/**
 * Tests that different values for the same configuration string key have the following order of
 * preference:
 *   1. index-specific options specified to createIndex().
 *   2. collection-wide options specified as "indexOptionDefaults" to createCollection().
 *   3. system-wide options specified by --wiredTigerIndexConfigString.
 */
(function() {
    'use strict';

    // Skip this test if not running with the "wiredTiger" storage engine.
    if (jsTest.options().storageEngine && jsTest.options().storageEngine !== 'wiredTiger') {
        jsTest.log('Skipping test because storageEngine is not "wiredTiger"');
        return;
    }

    // Use different values for the same configuration string key to test that index-specific
    // options override collection-wide options, and that collection-wide options override
    // system-wide options.
    var systemWideConfigString = 'split_pct=25,';
    var collectionWideConfigString = 'split_pct=30,';
    var indexSpecificConfigString = 'split_pct=35,';

    // Start up a mongod with system-wide defaults for index options and create a collection without
    // any additional options. Tests than an index without any additional options should take on the
    // system-wide defaults, whereas an index with additional options should override the
    // system-wide defaults.
    runTest({});

    // Start up a mongod with system-wide defaults for index options and create a collection with
    // additional options. Tests than an index without any additional options should take on the
    // collection-wide defaults, whereas an index with additional options should override the
    // collection-wide defaults.
    runTest({indexOptionDefaults: collectionWideConfigString});

    function runTest(collOptions) {
        var hasIndexOptionDefaults = collOptions.hasOwnProperty('indexOptionDefaults');

        var dbpath = MongoRunner.dataPath + 'wt_index_option_defaults';
        resetDbpath(dbpath);

        // Start a mongod with system-wide defaults for WiredTiger-specific index options.
        var conn = MongoRunner.runMongod({
            dbpath: dbpath,
            noCleanData: true,
            wiredTigerIndexConfigString: systemWideConfigString
        });
        assert.neq(null, conn, 'mongod was unable to start up');

        var testDB = conn.getDB('test');
        var cmdObj = {create: 'coll'};

        // Apply collection-wide defaults for WiredTiger-specific index options if any were
        // specified.
        if (hasIndexOptionDefaults) {
            cmdObj.indexOptionDefaults = {
                storageEngine: {
                    wiredTiger: {
                        configString: collOptions.indexOptionDefaults
                    }
                }
            };
        }
        assert.commandWorked(testDB.runCommand(cmdObj));

        // Create an index that does not specify any WiredTiger-specific options.
        assert.commandWorked(testDB.coll.createIndex({a: 1}, {name: 'without_options'}));

        // Create an index that specifies WiredTiger-specific index options.
        assert.commandWorked(testDB.coll.createIndex({b: 1}, {
            name: 'with_options',
            storageEngine: {wiredTiger: {configString: indexSpecificConfigString}}
        }));

        var collStats = testDB.runCommand({collStats: 'coll'});
        assert.commandWorked(collStats);

        checkIndexWithoutOptions(collStats.indexDetails);
        checkIndexWithOptions(collStats.indexDetails);

        MongoRunner.stopMongod(conn);

        function checkIndexWithoutOptions(indexDetails) {
            var indexSpec = getIndexSpecByName(testDB.coll, 'without_options');
            assert(!indexSpec.hasOwnProperty('storageEngine'),
                   'no storage engine options should have been set in the index spec: ' +
                   tojson(indexSpec));

            var creationString = indexDetails.without_options.creationString;
            if (hasIndexOptionDefaults) {
                assert.eq(-1, creationString.indexOf(systemWideConfigString),
                          'system-wide index option present in the creation string even though a ' +
                          'collection-wide option was specified: ' + creationString);
                assert.lte(0, creationString.indexOf(collectionWideConfigString),
                           'collection-wide index option not present in the creation string: ' +
                           creationString);
            } else {
                assert.lte(0, creationString.indexOf(systemWideConfigString),
                           'system-wide index option not present in the creation string: ' +
                           creationString);
                assert.eq(-1, creationString.indexOf(collectionWideConfigString),
                          'collection-wide index option present in creation string even though ' +
                          'it was not specified: ' + creationString);
            }

            assert.eq(-1, creationString.indexOf(indexSpecificConfigString),
                      'index-specific option present in creation string even though it was not' +
                      ' specified: ' + creationString);
        }

        function checkIndexWithOptions(indexDetails) {
            var indexSpec = getIndexSpecByName(testDB.coll, 'with_options');
            assert(indexSpec.hasOwnProperty('storageEngine'),
                   'storage engine options should have been set in the index spec: ' +
                   tojson(indexSpec));
            assert.docEq({wiredTiger: {configString: indexSpecificConfigString}},
                         indexSpec.storageEngine,
                         'WiredTiger index options not present in the index spec');

            var creationString = indexDetails.with_options.creationString;
            assert.eq(-1, creationString.indexOf(systemWideConfigString),
                      'system-wide index option present in the creation string even though an ' +
                      'index-specific option was specified: ' + creationString);
            assert.eq(-1, creationString.indexOf(collectionWideConfigString),
                      'system-wide index option present in the creation string even though an ' +
                      'index-specific option was specified: ' + creationString);
            assert.lte(0, creationString.indexOf(indexSpecificConfigString),
                       'index-specific option not present in the creation string: ' +
                       creationString);
        }
    }

    function getIndexSpecByName(coll, indexName) {
        var indexes = coll.getIndexes().filter(function(spec) {
            return spec.name === indexName;
        });
        assert.eq(1, indexes.length, 'index "' + indexName + '" not found');
        return indexes[0];
    }
})();