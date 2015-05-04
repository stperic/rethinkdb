////
// Tests the JavaScript driver using the nested function interface
/////

var assert = require('assert');
var path = require('path');

// -- settings

var driverPort = process.env.RDB_DRIVER_PORT || (process.argv[2] ? parseInt(process.argv[2], 10) : 28015);
var serverHost = process.env.RDB_SERVER_HOST || (process.argv[3] ? parseInt(process.argv[3], 10) : 'localhost');

var dbName = 'test';
var tableName = 'test';
var numRows = parseInt(process.env.TEST_ROWS) || 100;
var manyRows = parseInt(process.env.TEST_MANY_ROWS) || 10000; // Keep a "big" value to try hitting `maximum call stack exceed`

// -- load rethinkdb from the proper location

var r = require(path.resolve(__dirname, '..', 'importRethinkDB.js')).r;

// -- globals

var tbl = r.db(dbName).table(tableName);
var reqlConn = null;
var tableCursor = null;

// -- helper functions

var withConnection = function(fnct) {
    // ensure that the shared connection 'reqlConn' is valid
    if (fnct) {
        // nested function style
        return function(done) {
            r.expr(1).run(reqlConn, function(err) {
                if(err) {
                    reqlConn = null;
                    r.connect({host:serverHost, port:driverPort}, function(err, conn) {
                        if(err) { done(err) }
                        reqlConn = conn;
                        return fnct(done, reqlConn);
                    })
                } else {
                    return fnct(done, reqlConn);
                }
            });
        };
    } else {
        // promises style
        return r.expr(1).run(reqlConn) // check the connection
        .then(function() {
            return reqlConn;
        })
        // re-establish the connection if it was bad
        .catch(r.Error.RqlDriverError, r.Error.RqlRuntimeError, function(err) {
        	reqlConn = null;
        	return r.connect({host:serverHost, port:driverPort})
        	.then(function(conn) {
                // cache the new connection
                reqlConn = conn;
                return reqlConn;
        	});
        });
    }
}

var argErrorTest = function(expectedMsg) {
    return function(err) {
        return (err instanceof r.Error.RqlDriverError) && (expectedMsg == err.msg)
    }
}

// -- tests

describe('Cursor tests', function() {
    
    // setup
    before(function() {
        this.timeout(10000)
        
        // ensure db exists
        return withConnection()
        .then(function() {
            r.expr([dbName]).setDifference(r.dbList()).forEach(
                function(value) { return r.dbCreate(value); }
            ).run(reqlConn)
        })
        // ensure table exists
        .then(function() {
            return r.expr([tableName]).setDifference(r.db(dbName).tableList()).forEach(
                function(value) { return r.db(dbName).tableCreate(value); }
            ).run(reqlConn);
        })
        // clean out table
        .then(function() {
            return tbl.delete().run(reqlConn);
        })
        // add simple data
        .then(function() {
            return r.range(0, numRows).forEach(
                function(i) { return tbl.insert({'id':i}); }
            ).run(reqlConn);
        });
    });
    
    // ensure tableCursor is valid before each test
    beforeEach(function() { return withConnection(); });
    
    describe('nested function style', function() {
        
        it('open', function(done) {
            tbl.run(reqlConn, function(err, cur) {
                assert.ifError(err);
                assert.equal(cur.constructor.name, 'Cursor');
                done();
            });
        });
        
        it('next', function(done) {
            tbl.orderBy({index:'id'}).run(reqlConn, function(err, cur) {
                assert.ifError(err);
                cur.next(function(err, row) {
                    assert.ifError(err);
                    assert.deepEqual(row, {'id':0});
                    done();
                })
            })
        });
        
        it('toArray', function(done) {
            var expectedResult = [];
            for (var i = 0; i < numRows; i++) {
                expectedResult.push({'id':i});
            }
            tbl.orderBy({index:'id'}).run(reqlConn, function(err, cur) {
                cur.toArray(function(err, value) {
                    assert.ifError(err);
                    assert.deepEqual(value, expectedResult);
                    done();
                });
            });
        });
        
        it('each', function(done) {
            tbl.orderBy({index:'id'}).run(reqlConn, function(err, cur) {
                assert.ifError(err);
                var i = 0;
                cur.each(
                    function(err, row) {
                        assert.ifError(err);
                        assert.deepEqual(row, {'id':i});
                        i++;
                    },
                    function() {
                        assert.equal(i, numRows);
                        done();
                    }
                );
            });
        });
        
        it('close', function(done) {
            tbl.run(reqlConn, function(err, cur) {
                assert.ifError(err);
                cur.close(function(err) {
                    assert.ifError(err);
                    done();
                });
            });
        });
        
        it('missing argument on each', function(done) {
            tbl.run(reqlConn, function(err, cur) {
                assert.ifError(err);
                expectedMsg = 'Expected between 1 and 2 arguments but found 0.';
                assert.throws(function() { cur.each(); }, argErrorTest(expectedMsg));
                done();
            });
        });
        
        it('extra argument to toString', function(done) {
            tbl.run(reqlConn, function(err, cur) {
                assert.ifError(err);
                expectedMsg = 'Expected 0 arguments but found 1.';
                assert.throws(function() { cur.toString(1); }, argErrorTest(expectedMsg));
                done();
            });
        });
    });
});

describe('Array tests', function() {
    var testArray = [];
    for(var i = 0; i < numRows; i++) {
        testArray.push(i);
    }
    
    // ensure tableCursor is valid before each test
    beforeEach(function() { return withConnection(); });
    
    if('basic', function(done) {
        r(testArray).run(c, function(err, res) {
            assert.ifError(err);
            
            assert(res instanceof Array);
            assert.equal(res.length, numRows);
            assert.deepEqual(res, testArray);
            
            res.push(numRows);
            assert.equal(res[numRows], numRows);
            assert.equal(res.length, numRows + 1);
            done();
        });
    })
    
    it('next', function(done) {
         r(testArray).run(reqlConn, function(err, res) {
            res.next(function(err, value) {
                assert.equal(value, testArray[0]);
                done();
            });
         });
    });
    
    it('toArray', function(done) {
        r(testArray).run(reqlConn, function(err, res) {
            assert.ifError(err);
            assert.deepEqual(res, testArray);
            
            res.toArray(function(err, res2) {
                assert.ifError(err);
                assert.strictEqual(res, res2); // this should return the same object, not a copy
                done();
            });
        });
    });
    
    it('large array', function(done) {
        var bigArray = [];
        for(var i = 0; i < manyRows; i++) {
            bigArray.push(i);
        }
        r.expr(bigArray).run(reqlConn, function(err, res) {
            assert.ifError(err);
            
            var i = 0;
            res.each(
                function(err, value) {
                    assert.ifError(err);
                    assert.equal(value, bigArray[i])
                    i++;
                },
                function() {
                    assert.equal(i, manyRows);
                    done();
                }
            );
        });
    });
});
