/**
 * @file
 *
 * @brief
 *
 * @copyright BSD License (see LICENSE.md or https://www.libelektra.org)
 */

#include <kdbopts.h>

#include <stdlib.h>
#include <string.h>

#include <kdbease.h>
#include <kdbhelper.h>
#include <kdbmeta.h>

#include <kdberrors.h>

#ifdef _WIN32
static const char SEP_ENV_VALUE = ';';
#else
static const char SEP_ENV_VALUE = ':';
#endif

struct OptionData
{
	Key * specKey;
	const char * metaKey;
	const char * hasArg;
	const char * kind;
	const char * flagValue;
	const char * argName;
	bool hidden;
};

struct Specification
{
	KeySet * options;
	KeySet * keys;
	bool hasOpts;
	bool hasArgs;
};

static inline const char * keyGetMetaString (const Key * key, const char * meta)
{
	const Key * mk = keyGetMeta (key, meta);
	const char * value = mk == NULL ? NULL : keyString (mk);
	return value != NULL && value[0] == '\0' ? NULL : value;
}

static int addProcKey (KeySet * ks, const Key * key, Key * valueKey);
static KeySet * parseEnvp (const char ** envp);

static KeySet * parseArgs (KeySet * optionsSpec, int argc, const char ** argv, Key * errorKey);
static void setOption (Key * option, const char * value, bool repeated);

static Key * splitEnvValue (const Key * envKey);

static KeySet * ksMetaGetSingleOrArray (Key * key, const char * metaName);

static char * generateUsageLine (const char * progname, bool hasOpts, bool hasArgs);
static char * generateOptionsList (KeySet * keysWithOpts);

static bool processSpec (struct Specification * spec, KeySet * ks, Key * errorKey);
static bool processOptions (struct Specification * spec, Key * specKey, Key ** keyWithOpt, Key * errorKey);
static bool readOptionData (struct OptionData * optionData, Key * key, const char * metaKey, Key * errorKey);
static bool processShortOptSpec (struct Specification * spec, struct OptionData * optionData, Key ** keyWithOpt, char ** shortOptLine,
				 Key * errorKey);
static bool processLongOptSpec (struct Specification * spec, struct OptionData * optionData, Key ** keyWithOpt, char ** longOptLine,
				Key * errorKey);
static bool processEnvVars (KeySet * usedEnvVars, Key * specKey, Key ** keyWithOpt, Key * errorKey);

static int writeOptionValues (KeySet * ks, Key * keyWithOpt, KeySet * options, Key * errorKey);
static int writeEnvVarValues (KeySet * ks, Key * keyWithOpt, KeySet * envValues, Key * errorKey);
static void writeArgsValues (KeySet * ks, Key * keyWithOpt, KeySet * args);

static bool parseLongOption (KeySet * optionsSpec, KeySet * options, int argc, const char ** argv, int * index, Key * errorKey);
static bool parseShortOptions (KeySet * optionsSpec, KeySet * options, int argc, const char ** argv, int * index, Key * errorKey);

/**
 * This functions parses a specification of program options, together with a list of arguments
 * and environment variables to extract the option values.
 *
 * The options have to be defined in the metadata of keys in the spec namespace. If an option value
 * is found for any of the given keys, a new key with the same path but inside the proc namespace
 * will be inserted into @p ks. This enables a cascading lookup to find these values.
 *
 * If @p argv contains "-h" or "--help" @p ks will not be changed, instead the value of @p errorKey
 * will be set to a help message describing available options and 1 will be returned. The program
 * name used in this message is taken from `argv[0]`. If it contains a '/' only the part after the
 * last '/' will be used. The help message will contain description for each option. This description
 * is taken from `opt/help`, or if `opt/help` is not specified from `description`. The `opt/help` metadata
 * is specified PER KEY not per option, even if the key has multiple options. This is because all the
 * options for one key have to be equivalent and therefore only require one description.
 *
 * If `opt/nohelp` is set to "1", the option will not appear in the help message. The same applies to
 * `env`s and `env/nohelp`.
 *
 * If `opt/arg/help` is set, its value will be used as the argument name for long options. Otherwise
 * "ARG" will be used. This value can be set for each option individually.
 *
 * The help message can also be customised in two more ways:
 * 1. By setting the `help/usage` metadata on @p errorKey, you can use a custom usage line for the help
 *    message.
 * 2. By setting the `help/prefix` metadata on @p errorKey, you can insert text between the usage line
 *    and the description of options.
 * There is no `help/suffix` metadata, because you can easily append text to the help message yourself.
 *
 * Because "-h" and "--help" are reserved for the help message, neither can be used for anything else.
 *
 * To define a command line option set the `opt` meta-key to the short option. Only the first
 * character of the given value will be used ('\0' is not allowed). You can also set `opt/long`,
 * to use a long option. Each option can have a short version, a long version or both.
 * Per default an option is expected to have an argument. To change this behaviour set `opt/arg` to
 * either 'none' or 'optional' (the default is 'required'). The behaviour of short options, long
 * options, required and optional arguments is similar to getopt_long(3).
 *
 * A short option cannot be set to 'optional', as the only way to give an 'optional' argument is via
 * the "--opt=value" syntax of long options. If a long option with an 'optional' argument does not
 * have an argument, its value will be set as if `opt/arg` were 'none'. One difference to getopt_long(3)
 * is that options with 'optional' arguments can also have short versions. In this case the short version
 * is treated as if `opt/arg` were set to 'none', i.e. it cannot have an argument.
 *
 * If `opt/arg` is set to 'none' the the corresponding key will have the value "1", if the option is
 * provided. This value can be changed by setting `opt/flagvalue` to something else.
 *
 * A key can also be associated with multiple options. To achieve this, simply follow the instructions
 * above, but replace `opt` with `opt/#XXX` (XXX being the index of the option) in all keynames. Each
 * option can have a different setting for none/required/optional arguments and flagvalue too.
 *
 * In addition to command line options this function also supports environment variables. These are
 * specified with `env` (or `env/#XXX` if multiple are used).
 *
 * While you can specify multiple options (or environment variables) for a single key, only one of them
 * can be used at the same time. Using two or more options (or variables) that are all linked to the
 * same key, will result in an error.
 *
 * If the key for which the options are defined, has the basename '#', an option can be repeated.
 * All occurrences will be collected into an array. Environment variables obviously cannot be repeated,
 * instead a behaviour similar that used for PATH is adopted. On Windows the variable will be split at
 * each ';' character. On all other systems ':' is used as a separator.
 *
 * In case multiple versions (short, long, env-var) of an option are found, the order of precedence is:
 * <ul>
 * 	<li> Short options always win. </li>
 * 	<li> Long options are used if no short option is present. </li>
 * 	<li> If neither a long nor short option is found, environment variables are considered. </li>
 * </ul>
 *
 * Lastly, all remaining elements of @p argv will be collected into an array. You can access this array
 * by specifying `args = remaining` on a key with basename '#'. The array will be copied into this key. As is
 * the case with getopt(3) processing of options will stop if '--' is encountered in @p argv.
 *
 * NOTE: Both options and environment variables can only be specified on a single key. This is because
 * otherwise e.g. the help message ('opt/help', 'env/help') may be ambiguous. If you need to have the
 * value of one option/environment variable in multiple keys, you may use fallbacks.
 *
 * NOTE: Per default option processing DOES NOT stop, when a non-option string is encountered in @p argv.
 * If you want processing to stop, set the metadata `posixly = 1` on @p errorKey.
 *
 * @param ks	The KeySet containing the specification for the options.
 * @param argc	The number of strings in argv.
 * @param argv	The arguments to be processed.
 * @param envp	A list of environment variables. This needs to be a null-terminated list of
 * 		strings of the format 'KEY=VALUE'.
 * @param errorKey A key to store an error in, if one occurs.
 *
 * @retval 0	on success
 * @retval -1	on error, the error will be added to @p errorKey
 * @retval 1	if help option was found
 */
int elektraGetOpts (KeySet * ks, int argc, const char ** argv, const char ** envp, Key * errorKey)
{
	cursor_t initial = ksGetCursor (ks);

	struct Specification spec;
	if (!processSpec (&spec, ks, errorKey))
	{
		return -1;
	}

	KeySet * options = parseArgs (spec.options, argc, argv, errorKey);

	if (options == NULL)
	{
		ksDel (spec.options);
		ksDel (spec.keys);
		return -1;
	}

	Key * helpKey = ksLookupByName (options, "/short/h", 0);
	if (helpKey == NULL)
	{
		helpKey = ksLookupByName (options, "/long/help", 0);
	}

	if (helpKey != NULL)
	{
		// show help
		const char * progname = argv[0];
		char * lastSlash = strrchr (progname, '/');
		if (lastSlash != NULL)
		{
			progname = lastSlash + 1;
		}

		char * usage = generateUsageLine (progname, spec.hasOpts, spec.hasArgs);
		char * optionsText = generateOptionsList (spec.keys);

		keySetMeta (errorKey, "internal/libopts/help/usage", usage);
		keySetMeta (errorKey, "internal/libopts/help/options", optionsText);

		elektraFree (usage);
		elektraFree (optionsText);
		ksDel (options);
		ksDel (spec.options);
		ksDel (spec.keys);
		return 1;
	}
	ksDel (spec.options);

	KeySet * envValues = parseEnvp (envp);

	Key * argsParent = keyNew ("/args", KEY_END);
	KeySet * args = elektraArrayGet (argsParent, options);
	keyDel (argsParent);

	Key * keyWithOpt;
	ksRewind (spec.keys);
	while ((keyWithOpt = ksNext (spec.keys)) != NULL)
	{
		int result = writeOptionValues (ks, keyWithOpt, options, errorKey);
		if (result < 0)
		{
			ksDel (envValues);
			ksDel (options);
			ksDel (spec.keys);
			ksDel (args);
			return -1;
		}
		else if (result > 0)
		{
			continue;
		}

		result = writeEnvVarValues (ks, keyWithOpt, envValues, errorKey);
		if (result < 0)
		{
			ksDel (envValues);
			ksDel (options);
			ksDel (spec.keys);
			ksDel (args);
			return -1;
		}
		else if (result > 0)
		{
			continue;
		}

		writeArgsValues (ks, keyWithOpt, args);
	}

	ksDel (envValues);
	ksDel (options);
	ksDel (spec.keys);
	ksDel (args);

	ksSetCursor (ks, initial);

	return 0;
}

bool processSpec (struct Specification * spec, KeySet * ks, Key * errorKey)
{
	KeySet * usedEnvVars = ksNew (0, KS_END);
	spec->options = ksNew (
		2,
		keyNew ("/short/h", KEY_META, "hasarg", "none", KEY_META, "kind", "single", KEY_META, "flagvalue", "1", KEY_META, "longopt",
			"/long/help", KEY_END),
		keyNew ("/long/help", KEY_META, "hasarg", "none", KEY_META, "kind", "single", KEY_META, "flagvalue", "1", KEY_END), KS_END);
	spec->keys = ksNew (0, KS_END);

	spec->hasOpts = false;
	spec->hasArgs = false;

	ksRewind (ks);
	Key * cur;
	while ((cur = ksNext (ks)) != NULL)
	{
		if (keyGetNamespace (cur) != KEY_NS_SPEC)
		{
			continue;
		}

		Key * keyWithOpt = NULL;

		if (!processOptions (spec, cur, &keyWithOpt, errorKey))
		{
			ksDel (spec->options);
			ksDel (spec->keys);
			ksDel (usedEnvVars);
			return false;
		}

		if (!processEnvVars (usedEnvVars, cur, &keyWithOpt, errorKey))
		{
			ksDel (spec->options);
			ksDel (spec->keys);
			ksDel (usedEnvVars);
			return false;
		}

		const char * argsMeta = keyGetMetaString (cur, "args");
		if (argsMeta != NULL && elektraStrCmp (argsMeta, "remaining") == 0)
		{
			if (elektraStrCmp (keyBaseName (cur), "#") != 0)
			{
				ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
						    "'args=remaining' can only be set on array keys (basename = '#'). Offending key: %s",
						    keyName (cur));
				ksDel (spec->options);
				ksDel (spec->keys);
				ksDel (usedEnvVars);
				return false;
			}

			if (keyWithOpt == NULL)
			{
				keyWithOpt = keyNew (keyName (cur), KEY_END);
			}
			keySetMeta (keyWithOpt, "args", "remaining");
			spec->hasArgs = true;
		}

		if (keyWithOpt != NULL)
		{
			ksAppendKey (spec->keys, keyWithOpt);
		}
	}
	ksDel (usedEnvVars);

	return true;
}

/**
 * Extracts the help message from the @p errorKey used in elektraGetOpts().
 *
 * @param errorKey The same Key as passed to elektraGetOpts() as errorKey.
 * @param usage	   If this is not NULL, it will be used instead of the default usage line.
 * @param prefix   If this is not NULL, it will be inserted between the usage line and the options list.
 *
 * @return The full help message extracted from @p errorKey, or NULL if no help message was found.
 * The returned string has to be freed with elektraFree().
 */
char * elektraGetOptsHelpMessage (Key * errorKey, const char * usage, const char * prefix)
{
	if (usage == NULL)
	{
		usage = keyGetMetaString (errorKey, "internal/libopts/help/usage");
	}

	if (usage == NULL)
	{
		return NULL;
	}

	const char * options = keyGetMetaString (errorKey, "internal/libopts/help/options");
	if (options == NULL)
	{
		options = "";
	}

	return elektraFormat ("%s%s%s", usage, prefix == NULL ? "" : prefix, options);
}

// -------------
// static functions
// -------------

/**
 * Process the option specification for @p specKey.
 *
 * @retval true on success
 * @retval false on error
 */
bool processOptions (struct Specification * spec, Key * specKey, Key ** keyWithOpt, Key * errorKey)
{
	const char * optHelp = keyGetMetaString (specKey, "opt/help");
	const char * description = keyGetMetaString (specKey, "description");

	const char * help = optHelp != NULL ? optHelp : (description != NULL ? description : "");

	KeySet * opts = ksMetaGetSingleOrArray (specKey, "opt");
	if (opts == NULL)
	{
		const char * longOpt = keyGetMetaString (specKey, "opt/long");
		if (longOpt == NULL)
		{
			return true;
		}

		// no other way to create Key with name "opt"
		Key * k = keyNew ("/", KEY_META, "opt", "", KEY_END);
		opts = ksNew (2, keyNew ("/#", KEY_END), keyGetMeta (k, "opt"), KS_END);
		keyDel (k);
	}

	ksRewind (opts);
	ksNext (opts); // skip count
	Key * metaKey;

	char * shortOptLine = elektraFormat ("");
	char * longOptLine = elektraFormat ("");
	while ((metaKey = ksNext (opts)) != NULL)
	{
		struct OptionData optionData;

		if (!readOptionData (&optionData, specKey, keyName (metaKey), errorKey))
		{
			ksDel (opts);
			elektraFree (shortOptLine);
			elektraFree (longOptLine);
			return false;
		}


		if (!processShortOptSpec (spec, &optionData, keyWithOpt, &shortOptLine, errorKey))
		{
			ksDel (opts);
			elektraFree (shortOptLine);
			elektraFree (longOptLine);
			return false;
		}

		if (!processLongOptSpec (spec, &optionData, keyWithOpt, &longOptLine, errorKey))
		{
			ksDel (opts);
			elektraFree (shortOptLine);
			elektraFree (longOptLine);
			return false;
		}
	}

	if (*keyWithOpt != NULL)
	{
		if (strlen (shortOptLine) > 2)
		{
			shortOptLine[strlen (shortOptLine) - 2] = '\0'; // trim ", " of end
		}

		if (strlen (longOptLine) > 2)
		{
			longOptLine[strlen (longOptLine) - 2] = '\0'; // trim ", " of end
		}

		char * optsLinePart = elektraFormat ("%s%s%s", shortOptLine, strlen (longOptLine) > 0 ? ", " : "", longOptLine);
		elektraFree (shortOptLine);
		elektraFree (longOptLine);

		size_t length = strlen (optsLinePart);
		if (length > 0)
		{
			char * optsLine;
			if (length < 30)
			{
				optsLine = elektraFormat ("  %-28s%s", optsLinePart, help);
			}
			else
			{
				optsLine = elektraFormat ("  %s\n  %30s%s", optsLinePart, "", help);
			}

			keySetMeta (*keyWithOpt, "opt/help", optsLine);
			elektraFree (optsLine);
		}
		elektraFree (optsLinePart);
	}
	else
	{
		elektraFree (shortOptLine);
		elektraFree (longOptLine);
	}

	ksDel (opts);

	return true;
}

/**
 * Read the option data (i.e. hasarg, flagvalue, etc.) for the option
 * given by @p metaKey 's name from @p key.
 * @retval true on success
 * @retval false on error
 */
bool readOptionData (struct OptionData * optionData, Key * key, const char * metaKey, Key * errorKey)
{
	// two slashes in string because array index is inserted in-between
	char metaBuffer[ELEKTRA_MAX_ARRAY_SIZE + sizeof ("opt//flagvalue") + 1];
	strncpy (metaBuffer, metaKey, ELEKTRA_MAX_ARRAY_SIZE + 3); // 3 = opt/ - null byte from ELEKTRA_MAX_SIZE
	strncat (metaBuffer, "/arg", 11);			   // 11 = remaining space in metaBuffer

	const char * hasArg = keyGetMetaString (key, metaBuffer);
	if (hasArg == NULL)
	{
		hasArg = "required";
	}

	strncpy (metaBuffer, metaKey, ELEKTRA_MAX_ARRAY_SIZE + 3); // 3 = opt/ - null byte from ELEKTRA_MAX_SIZE
	strncat (metaBuffer, "/arg/help", 11);			   // 11 = remaining space in metaBuffer

	const char * argNameMeta = keyGetMetaString (key, metaBuffer);

	strncpy (metaBuffer, metaKey, ELEKTRA_MAX_ARRAY_SIZE + 3); // 3 = opt/ - null byte from ELEKTRA_MAX_SIZE
	strncat (metaBuffer, "/flagvalue", 11);			   // 11 = remaining space in metaBuffer

	const char * flagValue = keyGetMetaString (key, metaBuffer);
	if (flagValue == NULL)
	{
		flagValue = "1";
	}
	else if (elektraStrCmp (hasArg, "none") != 0 && elektraStrCmp (hasArg, "optional") != 0)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
				    "The flagvalue metadata can only be used, if the opt/arg metadata is set to 'none' or "
				    "'optional'. (key: %s)",
				    keyName (key));
		return false;
	}

	strncpy (metaBuffer, metaKey, ELEKTRA_MAX_ARRAY_SIZE + 3); // 3 = opt/ - null byte from ELEKTRA_MAX_SIZE
	strncat (metaBuffer, "/nohelp", 11);			   // 11 = remaining space in metaBuffer

	bool nohelp = false;
	const char * nohelpStr = keyGetMetaString (key, metaBuffer);
	if (nohelpStr != NULL && elektraStrCmp (nohelpStr, "1") == 0)
	{
		nohelp = true;
	}

	const char * kind = "single";
	if (elektraStrCmp (keyBaseName (key), "#") == 0)
	{
		kind = "array";
	}

	optionData->specKey = key;
	optionData->metaKey = metaKey;
	optionData->hasArg = hasArg;
	optionData->flagValue = flagValue;
	optionData->argName = argNameMeta;
	optionData->hidden = nohelp;
	optionData->kind = kind;

	return true;
}

/**
 * Process a possible short option specification on @p keyWithOpt.
 * The specification will be added to @p optionsSpec. The option
 * string will be added to @p shortOptLine.
 *
 * @retval true on success
 * @retval false on error
 */
bool processShortOptSpec (struct Specification * spec, struct OptionData * optionData, Key ** keyWithOpt, char ** shortOptLine,
			  Key * errorKey)
{
	Key * key = optionData->specKey;
	const char * hasArg = optionData->hasArg;
	const char * kind = optionData->kind;
	const char * flagValue = optionData->flagValue;
	bool nohelp = optionData->hidden;

	const char * shortOptStr = keyGetMetaString (optionData->specKey, optionData->metaKey);
	if (shortOptStr == NULL || shortOptStr[0] == '\0')
	{
		return true;
	}

	const char shortOpt = shortOptStr[0];

	if (shortOpt == '-')
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
				    "'-' cannot be used as a short option. It would collide with the "
				    "special string '--'. Offending key: %s",
				    keyName (key));
		return false;
	}

	if (shortOpt == 'h')
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
				    "'h' cannot be used as a short option. It would collide with the "
				    "help option '-h'. Offending key: %s",
				    keyName (key));
		return false;
	}

	Key * shortOptSpec = keyNew ("/short", KEY_META, "key", keyName (key), KEY_META, "hasarg", hasArg, KEY_META, "kind", kind, KEY_META,
				     "flagvalue", flagValue, KEY_END);
	keyAddBaseName (shortOptSpec, (char[]){ shortOpt, '\0' });

	Key * existing = ksLookupByName (spec->options, keyName (shortOptSpec), 0);
	if (existing != NULL)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
				    "The option '-%c' has already been specified for the key '%s'. Additional key: %s", shortOpt,
				    keyGetMetaString (existing, "key"), keyName (key));
		keyDel (shortOptSpec);
		keyDel (existing);
		return false;
	}

	ksAppendKey (spec->options, shortOptSpec);

	if (*keyWithOpt == NULL)
	{
		*keyWithOpt = keyNew (keyName (key), KEY_END);
	}
	elektraMetaArrayAdd (*keyWithOpt, "opt", keyName (shortOptSpec));

	if (!nohelp)
	{
		char * newShortOptLine = elektraFormat ("%s-%c, ", *shortOptLine, shortOpt);
		elektraFree (*shortOptLine);
		*shortOptLine = newShortOptLine;
	}

	if (!optionData->hidden)
	{
		spec->hasOpts = true;
	}

	return true;
}

/**
 * Process a possible long option specification on @p keyWithOpt.
 * The specification will be added to @p optionsSpec. The option
 * string will be added to @p longOptLine.
 *
 * @retval true on success
 * @retval false on error
 */
bool processLongOptSpec (struct Specification * spec, struct OptionData * optionData, Key ** keyWithOpt, char ** longOptLine,
			 Key * errorKey)
{
	Key * key = optionData->specKey;
	const char * hasArg = optionData->hasArg;
	const char * kind = optionData->kind;
	const char * flagValue = optionData->flagValue;
	const char * argName = optionData->argName;
	bool hidden = optionData->hidden;

	const char * longMeta = elektraFormat ("%s/long", optionData->metaKey);
	const char * longOpt = keyGetMetaString (key, longMeta);

	if (longOpt == NULL)
	{
		return true;
	}

	if (elektraStrCmp (longOpt, "help") == 0)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
				    "'help' cannot be used as a long option. It would collide with the "
				    "help option '--help'. Offending key: %s",
				    keyName (key));
		return false;
	}

	Key * longOptSpec = keyNew ("/long", KEY_META, "key", keyName (key), KEY_META, "hasarg", hasArg, KEY_META, "kind", kind, KEY_META,
				    "flagvalue", flagValue, KEY_END);
	keyAddBaseName (longOptSpec, longOpt);

	Key * existing = ksLookupByName (spec->options, keyName (longOptSpec), 0);
	if (existing != NULL)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
				    "The option '--%s' has already been specified for the key '%s'. Additional key: %s", longOpt,
				    keyGetMetaString (existing, "key"), keyName (key));
		keyDel (longOptSpec);
		return false;
	}

	ksAppendKey (spec->options, longOptSpec);

	if (*keyWithOpt == NULL)
	{
		*keyWithOpt = keyNew (keyName (key), KEY_END);
	}
	elektraMetaArrayAdd (*keyWithOpt, "opt", keyName (longOptSpec));

	if (!hidden)
	{
		char * argString = "";
		if (elektraStrCmp (hasArg, "required") == 0)
		{
			argString = argName == NULL ? "=ARG" : elektraFormat ("=%s", argName);
		}
		else if (elektraStrCmp (hasArg, "optional") == 0)
		{
			argString = argName == NULL ? "=[ARG]" : elektraFormat ("=[%s]", argName);
		}


		char * newLongOptLine = elektraFormat ("%s--%s%s, ", *longOptLine, longOpt, argString);
		elektraFree (*longOptLine);
		if (argName != NULL)
		{
			elektraFree (argString);
		}
		*longOptLine = newLongOptLine;
	}

	if (!optionData->hidden)
	{
		spec->hasOpts = true;
	}

	return true;
}

/**
 * Process possible environment variable specifications on @p keyWithOpt.
 * @retval true on success
 * @retval false on error
 */
bool processEnvVars (KeySet * usedEnvVars, Key * specKey, Key ** keyWithOpt, Key * errorKey)
{
	KeySet * envVars = ksMetaGetSingleOrArray (specKey, "env");
	if (envVars == NULL)
	{
		return true;
	}

	ksRewind (envVars);
	ksNext (envVars); // skip count
	Key * k;
	while ((k = ksNext (envVars)) != NULL)
	{
		const char * envVar = keyString (k);
		if (envVar == NULL)
		{
			continue;
		}

		Key * envVarKey = keyNew ("/", KEY_META, "key", keyName (specKey), KEY_END);
		keyAddBaseName (envVarKey, envVar);

		Key * existing = ksLookupByName (usedEnvVars, keyName (envVarKey), 0);
		if (existing != NULL)
		{
			ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_SPEC, errorKey,
					    "The environment variable '%s' has already been specified for the key "
					    "'%s'. Additional key: %s",
					    envVar, keyGetMetaString (existing, "key"), keyName (specKey));
			keyDel (envVarKey);
			keyDel (existing);
			ksDel (envVars);
			return false;
		}

		ksAppendKey (usedEnvVars, envVarKey);

		if (*keyWithOpt == NULL)
		{
			*keyWithOpt = keyNew (keyName (specKey), KEY_END);
		}
		elektraMetaArrayAdd (*keyWithOpt, "env", keyName (envVarKey));
	}

	ksDel (envVars);

	return true;
}

/**
 * Add keys to the proc namespace in @p ks for all options specified
 * on @p keyWithOpt. The env-vars are taken from @p envValues.
 * @retval -1 in case of error
 * @retval 0 if no key was added
 * @retval 1 if keys were added to @p ks
 */
int writeOptionValues (KeySet * ks, Key * keyWithOpt, KeySet * options, Key * errorKey)
{
	bool valueFound = false;

	KeySet * optMetas = elektraMetaArrayToKS (keyWithOpt, "opt");
	if (optMetas == NULL)
	{
		return 0;
	}

	ksRewind (optMetas);
	ksNext (optMetas); // skip count
	Key * optMeta;
	bool shortFound = false;
	while ((optMeta = ksNext (optMetas)) != NULL)
	{
		Key * optKey = ksLookupByName (options, keyString (optMeta), 0);
		bool isShort = strncmp (keyString (optMeta), "/short", 6) == 0;
		if (shortFound && !isShort)
		{
			// ignore long options, if a short one was found
			continue;
		}

		int res = addProcKey (ks, keyWithOpt, optKey);
		if (res == 0)
		{
			valueFound = true;
			if (isShort)
			{
				shortFound = true;
			}
		}
		else if (res < 0)
		{
			ELEKTRA_SET_ERRORF (
				ELEKTRA_ERROR_OPTS_ILLEGAL_USE, errorKey,
				"The option '%s%s' cannot be used, because another option has already been used for the key '%s'.",
				isShort ? "-" : "--", isShort ? (const char[]){ keyBaseName (optKey)[0], '\0' } : keyBaseName (optKey),
				keyName (keyWithOpt));
			ksDel (optMetas);
			return -1;
		}
	}

	ksDel (optMetas);

	return valueFound ? 1 : 0;
}

/**
 * Add keys to the proc namespace in @p ks for everything that is specified
 * by the 'env' metadata on @p keyWithOpt. The env-vars are taken from @p envValues.
 * @retval -1 in case of error
 * @retval 0 if no key was added
 * @retval 1 if keys were added to @p ks
 */
int writeEnvVarValues (KeySet * ks, Key * keyWithOpt, KeySet * envValues, Key * errorKey)
{
	bool valueFound = false;

	KeySet * envMetas = elektraMetaArrayToKS (keyWithOpt, "env");
	if (envMetas == NULL)
	{
		return 0;
	}

	ksRewind (envMetas);
	ksNext (envMetas); // skip count
	Key * envMeta;
	while ((envMeta = ksNext (envMetas)) != NULL)
	{
		Key * envKey = ksLookupByName (envValues, keyString (envMeta), 0);
		Key * envValueKey = splitEnvValue (envKey);

		int res = addProcKey (ks, keyWithOpt, envValueKey);
		if (res < 0)
		{
			ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_USE, errorKey,
					    "The environment variable '%s' cannot be used, because another variable has "
					    "already been used for the key '%s'.",
					    keyBaseName (envKey), keyName (keyWithOpt));
			keyDel (envValueKey);
			ksDel (envMetas);
			return -1;
		}
		else if (res == 0)
		{
			valueFound = true;
		}
		keyDel (envValueKey);
	}
	ksDel (envMetas);

	return valueFound ? 1 : 0;
}

/**
 * Add keys to the proc namespace in @p ks for everything that is specified
 * by the 'args' metadata on @p keyWithOpt. The args are taken from @p args.
 */
void writeArgsValues (KeySet * ks, Key * keyWithOpt, KeySet * args)
{
	const char * argsMeta = keyGetMetaString (keyWithOpt, "args");
	if (argsMeta == NULL || elektraStrCmp (argsMeta, "remaining") != 0)
	{
		return;
	}

	Key * procKey = keyNew ("proc", KEY_END);
	keyAddName (procKey, strchr (keyName (keyWithOpt), '/'));

	Key * insertKey = keyDup (procKey);

	ksRewind (args);
	Key * arg = NULL;
	while ((arg = ksNext (args)) != NULL)
	{
		elektraArrayIncName (insertKey);

		Key * k = keyDup (insertKey);
		keySetString (k, keyString (arg));
		ksAppendKey (ks, k);
	}

	keySetBaseName (procKey, NULL); // remove #
	keySetString (procKey, keyBaseName (insertKey));
	ksAppendKey (ks, procKey);
	keyDel (insertKey);
}

/**
 * Taken a key whose value is an environment variable like array (like $PATH)
 * and turn it into a Key with either a string value or a metadata array 'values'
 * containing the value of the variable.
 */
Key * splitEnvValue (const Key * envKey)
{
	if (envKey == NULL)
	{
		return NULL;
	}

	Key * valueKey = keyNew (keyName (envKey), KEY_END);

	char * envValue = elektraStrDup (keyString (envKey));
	char * curEnvValue = envValue;

	char * c = strchr (curEnvValue, SEP_ENV_VALUE);
	if (c == NULL)
	{
		keySetString (valueKey, curEnvValue);
	}
	else
	{
		keySetString (valueKey, NULL);

		char * lastEnvValue = curEnvValue;
		while (c != NULL)
		{
			*c = '\0';

			elektraMetaArrayAdd (valueKey, "values", curEnvValue);

			curEnvValue = c + 1;
			lastEnvValue = curEnvValue;
			c = strchr (curEnvValue, SEP_ENV_VALUE);
		}
		elektraMetaArrayAdd (valueKey, "values", lastEnvValue);
	}

	elektraFree (envValue);

	return valueKey;
}

/**
 * @param replace if set to true pre-existing values will be replaced
 * @retval 0 if a proc key was added to ks
 * @retval -1 on pre-existing value, except if key is an array key, or replace == true then 0
 * @retval 1 on NULL pointers and failed insertion
 */
int addProcKey (KeySet * ks, const Key * key, Key * valueKey)
{
	if (ks == NULL || key == NULL || valueKey == NULL)
	{
		return 1;
	}

	Key * procKey = keyNew ("proc", KEY_END);
	keyAddName (procKey, strchr (keyName (key), '/'));

	bool isArrayKey = elektraStrCmp (keyBaseName (procKey), "#") == 0;
	if (isArrayKey)
	{
		keySetBaseName (procKey, NULL); // remove # (for lookup)
	}


	Key * existing = ksLookupByName (ks, keyName (procKey), 0);
	if (existing != NULL && strlen (keyString (existing)) > 0)
	{
		keyDel (procKey);
		return -1;
	}

	if (isArrayKey)
	{
		Key * insertKey = keyDup (procKey);
		keyAddBaseName (insertKey, "#");
		KeySet * values = elektraMetaArrayToKS (valueKey, "values");
		if (values == NULL)
		{
			keyDel (procKey);
			keyDel (insertKey);
			return 1;
		}

		ksRewind (values);
		Key * cur;
		ksNext (values); // skip count
		while ((cur = ksNext (values)) != NULL)
		{
			elektraArrayIncName (insertKey);

			Key * k = keyDup (insertKey);
			keySetString (k, keyString (cur));
			ksAppendKey (ks, k);
		}

		keySetString (procKey, keyBaseName (insertKey));
		keyDel (insertKey);
		ksDel (values);
	}
	else
	{
		keySetString (procKey, keyString (valueKey));
	}

	return ksAppendKey (ks, procKey) > 0 ? 0 : 1;
}

KeySet * parseEnvp (const char ** envp)
{
	KeySet * ks = ksNew (0, KS_END);

	const char ** cur = envp;
	while (*cur != NULL)
	{
		const char * eq = strchr (*cur, '=');
		Key * key = keyNew ("/", KEY_VALUE, eq + 1, KEY_END);
		size_t len = eq - *cur;
		char * name = elektraStrNDup (*cur, len + 1);
		name[len] = '\0';
		keyAddBaseName (key, name);
		ksAppendKey (ks, key);
		elektraFree (name);

		cur++;
	}

	return ks;
}

KeySet * parseArgs (KeySet * optionsSpec, int argc, const char ** argv, Key * errorKey)
{
	const char * posixlyStr = keyGetMetaString (errorKey, "posixly");
	bool posixly = false;
	if (posixlyStr != NULL && elektraStrCmp (posixlyStr, "1") == 0)
	{
		posixly = true;
	}

	Key * argKey = keyNew ("/args/#", KEY_END);

	KeySet * options = ksNew (0, KS_END);
	int i;
	for (i = 1; i < argc; ++i)
	{
		const char * cur = argv[i];
		if (cur[0] == '-')
		{
			// possible option
			if (cur[1] == '-')
			{
				if (cur[2] == '\0')
				{
					// end of options
					++i; // skip --
					break;
				}

				if (!parseLongOption (optionsSpec, options, argc, argv, &i, errorKey))
				{
					keyDel (argKey);
					ksDel (options);
					return NULL;
				}

				continue;
			}

			if (!parseShortOptions (optionsSpec, options, argc, argv, &i, errorKey))
			{
				keyDel (argKey);
				ksDel (options);
				return NULL;
			}
		}
		else
		{
			// not an option
			if (posixly)
			{
				break;
			}

			elektraArrayIncName (argKey);
			Key * newArgKey = keyDup (argKey);
			keySetString (newArgKey, cur);
			ksAppendKey (options, newArgKey);
		}
	}

	// collect rest of argv
	for (; i < argc; ++i)
	{
		elektraArrayIncName (argKey);
		Key * newArgKey = keyDup (argKey);
		keySetString (newArgKey, argv[i]);
		ksAppendKey (options, newArgKey);
	}

	ksAppendKey (options, keyNew ("/args", KEY_VALUE, keyBaseName (argKey), KEY_END));
	keyDel (argKey);

	return options;
}

bool parseShortOptions (KeySet * optionsSpec, KeySet * options, int argc, const char ** argv, int * index, Key * errorKey)
{
	int i = *index;
	for (const char * c = &argv[i][1]; *c != '\0'; ++c)
	{

		Key * shortOpt = keyNew ("/short", KEY_END);
		keyAddBaseName (shortOpt, (char[]){ *c, '\0' });

		Key * optSpec = ksLookupByName (optionsSpec, keyName (shortOpt), 0);

		if (optSpec == NULL)
		{
			ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_UNKNOWN_OPTION, errorKey, "Unknown short option: -%c",
					    keyBaseName (shortOpt)[0]);
			keyDel (shortOpt);
			keyDel (optSpec);
			return false;
		}

		const char * hasArg = keyGetMetaString (optSpec, "hasarg");
		const char * kind = keyGetMetaString (optSpec, "kind");
		const char * flagValue = keyGetMetaString (optSpec, "flagvalue");

		bool repeated = elektraStrCmp (kind, "array") == 0;

		Key * option = ksLookupByName (options, keyName (shortOpt), 0);
		if (option == NULL)
		{
			option = keyNew (keyName (shortOpt), KEY_META, "key", keyGetMetaString (optSpec, "key"), KEY_END);
			ksAppendKey (options, option);
		}
		else if (!repeated)
		{
			ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_USE, errorKey, "This option cannot be repeated: -%c",
					    keyBaseName (shortOpt)[0]);
			keyDel (shortOpt);
			keyDel (optSpec);
			return false;
		}
		keyDel (optSpec);

		bool last = false;
		if (elektraStrCmp (hasArg, "required") == 0)
		{
			if (*(c + 1) == '\0')
			{
				if (i >= argc - 1)
				{
					ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_USE, errorKey,
							    "Missing argument for short option: -%c", keyBaseName (shortOpt)[0]);
					keyDel (shortOpt);
					keyDel (option);
					return false;
				}
				// use next as arg and skip
				setOption (option, argv[++i], repeated);
				*index = i;
			}
			else
			{
				// use rest as argument
				setOption (option, c + 1, repeated);
				last = true;
			}
		}
		else
		{
			// use flag value
			setOption (option, flagValue, repeated);
		}
		keyDel (shortOpt);

		keySetMeta (option, "short", "1");
		ksAppendKey (options, option);

		if (last)
		{
			break;
		}
	}

	return true;
}

bool parseLongOption (KeySet * optionsSpec, KeySet * options, int argc, const char ** argv, int * index, Key * errorKey)
{
	int i = *index;
	Key * longOpt = keyNew ("/long", KEY_END);

	char * opt = elektraStrDup (&argv[i][2]);
	char * eq = strchr (opt, '=');
	size_t argStart = 0;
	if (eq != NULL)
	{
		// mark end of option
		*eq = '\0';
		argStart = eq - opt + 3;
	}

	keyAddBaseName (longOpt, opt);
	elektraFree (opt);

	// lookup spec
	Key * optSpec = ksLookupByName (optionsSpec, keyName (longOpt), 0);

	if (optSpec == NULL)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_UNKNOWN_OPTION, errorKey, "Unknown long option: --%s", keyBaseName (longOpt));
		keyDel (longOpt);
		return false;
	}

	const char * hasArg = keyGetMetaString (optSpec, "hasarg");
	const char * kind = keyGetMetaString (optSpec, "kind");
	const char * flagValue = keyGetMetaString (optSpec, "flagvalue");

	bool repeated = elektraStrCmp (kind, "array") == 0;

	Key * option = ksLookupByName (options, keyName (longOpt), 0);
	if (option == NULL)
	{
		option = keyNew (keyName (longOpt), KEY_META, "key", keyGetMetaString (optSpec, "key"), KEY_END);
		ksAppendKey (options, option);
	}
	else if (!repeated)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_USE, errorKey, "This option cannot be repeated: --%s",
				    keyBaseName (longOpt));
		keyDel (longOpt);
		keyDel (optSpec);
		return false;
	}
	else if (keyGetMetaString (option, "short") != NULL)
	{
		keyDel (longOpt);
		keyDel (optSpec);
		// short option found already ignore long version
		return true;
	}
	keyDel (optSpec);

	if (elektraStrCmp (hasArg, "required") == 0)
	{
		// extract argument
		if (argStart > 0)
		{
			// use '=' arg
			setOption (option, &argv[i][argStart], repeated);
		}
		else
		{
			if (i >= argc - 1)
			{
				ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_USE, errorKey, "Missing argument for long option: --%s",
						    keyBaseName (longOpt));
				keyDel (longOpt);
				return false;
			}
			// use next as arg and skip
			setOption (option, argv[++i], repeated);
			*index = i;
		}
	}
	else if (elektraStrCmp (hasArg, "optional") == 0)
	{
		if (argStart > 0)
		{
			// only use '=' argument
			setOption (option, &argv[i][argStart], repeated);
		}
		else if (flagValue != NULL)
		{
			// use flag value
			setOption (option, flagValue, repeated);
		}
	}
	else
	{
		if (argStart > 0)
		{
			ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_OPTS_ILLEGAL_USE, errorKey, "This option cannot have an argument: --%s",
					    keyBaseName (longOpt));
			keyDel (longOpt);
			return false;
		}

		// use flag value
		setOption (option, flagValue, repeated);
	}
	keyDel (longOpt);

	return true;
}

void setOption (Key * option, const char * value, bool repeated)
{
	if (repeated)
	{
		elektraMetaArrayAdd (option, "values", value);
	}
	else
	{
		keySetString (option, value);
	}
}

KeySet * ksMetaGetSingleOrArray (Key * key, const char * metaName)
{
	const Key * k = keyGetMeta (key, metaName);
	if (k == NULL)
	{
		return NULL;
	}

	const char * value = keyString (k);
	if (value[0] != '#')
	{
		// add dummy key to mimic elektraMetaArrayToKS
		return ksNew (2, keyNew ("/#", KEY_END), k, KS_END);
	}

	Key * testKey = keyDup (k);
	keyAddBaseName (testKey, keyString (k));

	const Key * test = keyGetMeta (key, keyName (testKey));
	keyDel (testKey);

	if (test == NULL)
	{
		// add dummy key to mimic elektraMetaArrayToKS
		return ksNew (2, keyNew ("/#", KEY_END), k, KS_END);
	}

	return elektraMetaArrayToKS (key, metaName);
}

/**
 * Generate help message from optionsSpec.
 *
 * @return a newly allocated string, must be freed with elektraFree()
 */
char * generateUsageLine (const char * progname, bool hasOpts, bool hasArgs)
{
	return elektraFormat ("Usage: %s%s%s\n", progname, hasOpts ? " [OPTION]..." : "", hasArgs ? " [ARG]..." : "");
}

char * generateOptionsList (KeySet * keysWithOpts)
{
	if (ksGetSize (keysWithOpts) == 0)
	{
		return elektraStrDup ("");
	}

	cursor_t cursor = ksGetCursor (keysWithOpts);

	char * optionsList = elektraFormat ("OPTIONS");

	Key * cur = NULL;
	ksRewind (keysWithOpts);
	while ((cur = ksNext (keysWithOpts)) != NULL)
	{
		const char * optLine = keyGetMetaString (cur, "opt/help");
		if (optLine != NULL)
		{
			char * newOptionsList = elektraFormat ("%s\n%s", optionsList, optLine);
			elektraFree (optionsList);
			optionsList = newOptionsList;
		}
	}

	char * newOptionsList = elektraFormat ("%s\n", optionsList);
	elektraFree (optionsList);
	optionsList = newOptionsList;

	ksSetCursor (keysWithOpts, cursor);
	return optionsList;
}
