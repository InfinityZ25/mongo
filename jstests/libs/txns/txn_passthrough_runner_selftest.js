// Sanity test for the override logic in network_error_and_txn_override.js. We use the profiler to
// check that operation is not visible immediately, but is visible after the transaction commits.

(function() {
'use strict';

const testName = jsTest.name();

// Use a unique db for every test so burn_in_tests can run this test multiple times.
db = db.getSiblingDB('txn_self_test' + Random.srand());

// Profile all commands.
db.setProfilingLevel(2);

const coll = db[testName];

assert.commandWorked(coll.insert({x: 1}));
/* TODO(SERVER-47835) unblacklist
let commands = db.system.profile.find().toArray();
// Check that the insert is not visible because the txn has not committed.
assert.eq(commands.length, 0);
*/
// Use a dummy, unrelated operation to signal the txn runner to commit the transaction.
assert.commandWorked(db.runCommand({ping: 1}));

let commands = db.system.profile.find().toArray();
// Assert the insert is now visible.
assert.eq(commands.length, 1);
/* TODO(SERVER-47835) replace above assertion with below assertion.
assert.eq(commands.length, 2);*/
/* TODO(SERVER-47835) uncomment
assert.eq(commands[1].command.find, 'system.profile');*/
assert.eq(commands[0].command.insert, testName);
})();
