/***************************************************************************
 *          test_ks.c  -  KeySet struct test suite
 *                  -------------------
 *  begin                : Thu Dez 12 2006
 *  copyright            : (C) 2006 by Markus Raab
 *  email                : sizon5@gmail.com
 ****************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the BSD License (revised).                      *
 *                                                                         *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <tests.h>

void test_ksNew()
{
	KeySet *ks=0;
	KeySet * keys = ksNew (15,0);
	KeySet * config;

	printf("Test ks creation\n");
	exit_if_fail((ks=ksNew(0)) != 0, "could not create new keyset");

	succeed_if (ksAppendKey(ks,keyNew(0)) == -1, "could append a key with no name");
	succeed_if (ksAppendKey(ks,keyNew(0)) == -1, "could append a key with no name");
	succeed_if (ksAppendKey(ks,keyNew(0)) == -1, "could append a key with no name");
	succeed_if(ksGetSize(ks) == 0, "size not correct after 3 keys");

	KeySet *ks2=ksNew (0);
	ksCopy (ks2, ks);
	succeed_if(ksGetSize(ks2) == 0, "size not correct after 3 keys");

	succeed_if (ksAppendKey(ks,keyNew(0)) == -1, "could append a key with no name");
	succeed_if (ksAppendKey(ks,keyNew(0)) == -1, "could append a key with no name");
	succeed_if (ksAppendKey(ks,keyNew(0)) == -1, "could append a key with no name");
	succeed_if(ksGetSize(ks) == 0, "could not append 3 more keys");

	ksCopy (ks2, ks);
	succeed_if(ksGetSize(ks2) == 0, "could not append 3 more keys");

	ksClear (ks2); // useless, just test for double free
	ksCopy (ks2, ks);
	succeed_if(ksGetSize(ks2) == 0, "could not append 3 more keys");

	succeed_if(ksDel(ks) == 0, "could not delete keyset");


	succeed_if(ksGetSize(keys) == 0, "could not append 3 more keys");
	succeed_if(ksGetAlloc(keys) == 16, "allocation size wrong");
	succeed_if(ksDel(keys) == 0, "could not delete keyset");

	config = ksNew (100,
		keyNew ("user/sw/app/fixedConfiguration/key1", KEY_VALUE, "value1", 0),
		keyNew ("user/sw/app/fixedConfiguration/key2", KEY_VALUE, "value2", 0),
		keyNew ("user/sw/app/fixedConfiguration/key3", KEY_VALUE, "value3", 0),0);
	succeed_if(ksGetSize(config) == 3, "could not append 3 keys in keyNew");
	succeed_if(ksGetAlloc(config) == 100, "allocation size wrong");
	keyDel (ksPop (config));
	succeed_if(ksGetAlloc(config) == 50, "allocation size wrong");
	keyDel (ksPop (config));
	succeed_if(ksGetAlloc(config) == 25, "allocation size wrong");
	keyDel (ksPop (config));
	succeed_if(ksGetAlloc(config) == 16, "allocation size wrong");
	succeed_if(ksDel(config) == 0, "could not delete keyset");

	config = ksNew (10,
		keyNew ("user/sw/app/fixedConfiguration/key1", KEY_VALUE, "value1", 0),
		keyNew ("user/sw/app/fixedConfiguration/key2", KEY_VALUE, "value2", 0),
		keyNew ("user/sw/app/fixedConfiguration/key3", KEY_VALUE, "value1", 0),
		keyNew ("user/sw/app/fixedConfiguration/key4", KEY_VALUE, "value3", 0),0);

	succeed_if(ksGetSize(config) == 4, "could not append 5 keys in keyNew");
	succeed_if(ksGetAlloc(config) == 16, "allocation size wrong");
	ksAppendKey(config, keyNew ("user/sw/app/fixedConfiguration/key6", KEY_VALUE, "value4", 0));

	ksClear (ks2);
	ksCopy (ks2, config);
	compare_keyset (config, ks2, 0, 0);

	succeed_if(ksDel(config) == 0, "could not delete keyset");
	succeed_if(ksDel(ks2) == 0, "could not delete keyset");
}

void test_ksDuplicate()
{
	printf ("Test bug duplicate\n");
	KeySet *ks = ksNew(0);

	succeed_if (ksAppendKey (ks, keyNew("system/duplicate", KEY_VALUE, "abc", KEY_END)) == 1, "could not append key");
	succeed_if (!strcmp (keyValue(ksLookupByName(ks, "system/duplicate", 0)), "abc"), "wrong value for inserted key");

	succeed_if (ksAppendKey (ks, keyNew("system/duplicate", KEY_VALUE, "xyz", KEY_END)) == 1, "could not append duplicate key");
	succeed_if (!strcmp (keyValue(ksLookupByName(ks, "system/duplicate", 0)), "xyz"), "wrong value for inserted key");

	ksDel (ks);
}

void test_ksLookupCase()
{
	printf ("Test bug lookup with case\n");
	KeySet *ks = ksNew(32,
			keyNew("system/ay/key", KEY_VALUE, "aykey", KEY_END),
			keyNew("system/mY/kex", KEY_VALUE, "mykex", KEY_END),
			keyNew("system/xy/key", KEY_VALUE, "xykey", KEY_END),
			keyNew("system/My/key", KEY_VALUE, "Mykey", KEY_END),
			KS_END);
	Key *found = ksLookupByName (ks, "system/my/key", KDB_O_NOCASE);
	succeed_if (found != 0, "could not find key (binary search fails when ignoring case)");
	ksDel (ks);
}

void test_ksLookupOwner()
{
	printf ("Test bug lookup with owner\n");
	Key *found = 0;
	KeySet *ks = ksNew(32,
			keyNew("user:fritz/my/key", KEY_VALUE, "fritz", KEY_END),
			keyNew("user:frotz/my/key", KEY_VALUE, "frotz", KEY_END),
			keyNew("user/my/key", KEY_VALUE, "current", KEY_END), KS_END);

	found = ksLookupByName (ks, "user/my/key", KDB_O_WITHOWNER);
	succeed_if (found != 0, "could not find key");
	succeed_if (!strcmp (keyValue(found), "current"), "got wrong key");

	found = ksLookupByName (ks, "user:fritz/my/key", KDB_O_WITHOWNER);
	succeed_if (found != 0, "could not find key");
	succeed_if (!strcmp (keyValue(found), "fritz"), "got wrong key");

	found = ksLookupByName (ks, "user:frotz/my/key", KDB_O_WITHOWNER);
	succeed_if (found != 0, "could not find key");
	succeed_if (!strcmp (keyValue(found), "frotz"), "got wrong key");

	found = ksLookupByName (ks, "user:fretz/my/key", KDB_O_WITHOWNER);
	succeed_if (found == 0, "found non existing key");

	found = ksLookupByName (ks, "user/my/key", 0);
	succeed_if (found != 0, "could not find key");
	succeed_if (!strcmp (keyValue(found), "fritz"), "binary search seems to be non-deterministic");

	found = ksLookupByName (ks, "user:fritz/my/key", 0);
	succeed_if (found != 0, "could not find key");
	succeed_if (!strcmp (keyValue(found), "fritz"), "binary search seems to be non-deterministic");

	found = ksLookupByName (ks, "user:frotz/my/key", 0);
	succeed_if (found != 0, "could not find key");
	succeed_if (!strcmp (keyValue(found), "fritz"), "binary search seems to be non-deterministic");

	found = ksLookupByName (ks, "user:fretz/my/key", 0);
	succeed_if (found != 0, "could not find key");
	succeed_if (!strcmp (keyValue(found), "fritz"), "binary search seems to be non-deterministic");

	ksDel (ks);
}

void test_ksHole()
{
	printf ("Test holes in keysets\n");
	KeySet *ks = ksNew(0);

	succeed_if (ksAppendKey (ks, keyNew("system/sw/new", KEY_VALUE, "abc", KEY_END)) == 1, "could not append key");
	succeed_if (ksAppendKey (ks, keyNew("system/sw/new/sub", KEY_VALUE, "xyz", KEY_END)) == 2, "could not append key");
	succeed_if (ksAppendKey (ks, keyNew("system/sw/new/mis/sub", KEY_VALUE, "xyz", KEY_END)) == -1,
			"could append key which makes a hole");

	ksDel (ks);

}


int main(int argc, char** argv)
{
	printf("KEYSET       TESTS\n");
	printf("==================\n\n");

	init (argc, argv);

	test_ksNew();
	test_ksDuplicate();

	// TODO Bugs
	// test_ksLookupCase();
	// test_ksLookupOwner();

	test_ksHole();

	printf("\ntest_ks RESULTS: %d test(s) done. %d error(s).\n", nbTest, nbError);

	return nbError;
}

