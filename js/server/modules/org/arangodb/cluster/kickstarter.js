/*jslint indent: 2, nomen: true, maxlen: 120, sloppy: true, vars: true, white: true, plusplus: true, stupid: true, continue: true */
/*global module, require, exports, ArangoAgency, SYS_TEST_PORT */

////////////////////////////////////////////////////////////////////////////////
/// @brief Cluster kickstart functionality
///
/// @file js/server/modules/org/arangodb/cluster/kickstarter.js
///
/// DISCLAIMER
///
/// Copyright 2014 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Max Neunhoeffer
/// @author Copyright 2014, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

// -----------------------------------------------------------------------------
// --SECTION--                                        Kickstarter functionality
// -----------------------------------------------------------------------------

// possible config attributes (for defaults see below):
//   .agencyPrefix            prefix for this cluster in agency
//   .numberOfAgents          number of agency servers
//   .numberOfDBservers       number of DB servers
//   .startSecondaries        boolean, whether or not to use secondaries
//   .numberOfCoordinators    number of coordinators
//   .DBserverIDs             a list of DBserver IDs, the defaults will
//                            be appended to this list here
//   .coordinatorIDs          a list of coordinator IDs, the defaults will
//                            be appended to this list here
//   .dataPath                a file system path to the directory in which
//                            all the data directories of agents or servers
//                            live, this can be relative or even empty, which
//                            is equivalent to "./", please end with a slash
//                            if not empty
//   .dispatchers             an list of pairs of strings, the first entry
//                            is an ID, the second is an endpoint or "me"
//                            standing for the local `arangod` itself.
//                            this list can be empty in which case
//                            ["me","me"] is automatically added.
// some port lists:
//   for these the following rules apply:
//     every list overwrites the default list
//     when running out of numbers the kickstarter increments the last one
//       used by one for every port needed
//     
//   .agentExtPorts           a list port numbers to use for the
//                            external ports of agents,
//   .agentIntPorts           a list port numbers to use for the
//                            internal ports of agents,
//   .DBserverPorts           a list ports to try to use for DBservers
//   .coordinatorPorts        a list ports to try to use for coordinators
//  

// Examples of commands produced:

var x3 = 
  {
    "dispatchers": { "machine1": "tcp://192.168.173.101:8529",
                     "machine2": "tcp://192.168.173.102:8529",
                     "machine3": "tcp://192.168.173.103:8529" },
    "commands": [
      { "action": "startAgent", "dispatcher": "machine1", 
                                "ports":[4001,7001] },
      { "action": "startAgent",  "dispatcher": "machine2", 
                                 "ports":[4001,7001],
                                 "peers":["tcp://192.168.173.101:7001"] },
      { "action": "startAgent",  "dispatcher": "machine3", 
                                 "ports":[4001,7001],
                                 "peers":["tcp://192.168.173.101:7001",
                                          "tcp://192.168.173.102:7001"] },
      { "action": "sendConfiguration", "agency": "tcp://192.168.173.101:4001",
           "data" :
           { "mueller":
               { "Target":
                   { "MapIDToEndpoint":
                       { "Pavel": '"tcp://192.168.173.101:8531"',
                         "Perry": '"tcp://192.168.173.102:8531"',
                         "Pancho": '"tcp://192.168.173.103:8531"',
                         "Claus": '"tcp://192.168.173.101:8530"',
                         "Chantalle": '"tcp://192.168.173.102:8530"',
                         "Claire": '"tcp://192.168.173.103:8530"' },
                     "Lock": '"UNLOCKED"',
                     "Version": '"1"',
                     "DBServers":
                       { "Pavel": '"tcp://192.168.173.101:8531"',
                         "Perry": '"tcp://192.168.173.102:8531"',
                         "Pancho": '"tcp://192.168.173.103:8531"' },
                     "Coordinators":
                       { "Claus": '"tcp://192.168.173.101:8530"',
                         "Chantalle": '"tcp://192.168.173.102:8530"',
                         "Claire": '"tcp://192.168.173.103:8530"' },
                     "Databases":
                       { "_system": {} },
                     "Collections":
                       { "_system": {} } },
                 "Plan":
                   { "Lock": '"UNLOCKED"',
                     "Version": '"1"',
                     "DBServers":
                       { "Pavel": '"tcp://192.168.173.101:8531"',
                         "Perry": '"tcp://192.168.173.102:8531"',
                         "Pancho": '"tcp://192.168.173.103:8531"' },
                     "Coordinators":
                       { "Claus": '"tcp://192.168.173.101:8530"',
                         "Chantalle": '"tcp://192.168.173.102:8530"',
                         "Claire": '"tcp://192.168.173.103:8530"' },
                     "Databases":
                       { "_system": {} },
                     "Collections":
                       { "_system": {} } },
                 "Current":
                   { "Lock": '"UNLOCKED"',
                     "Version": '"1"',
                     "DBServers": {},
                     "Coordinators": {},
                     "Databases":
                       { "_system": {} },
                     "Collections":
                       { "_system": {} },
                     "ServersRegistered": 
                       { "Version": '"1"' },
                     "ShardsCopied": {} },
                 "Sync":
                   { "ServerStates": {},
                     "Problems": {},
                     "LatestID": '"0"',
                     "Commands": 
                       { "Pavel": '"SERVE"',
                         "Perry": '"SERVE"',
                         "Pancho": '"SERVE"' },
                     "HeartbeatInvervalMs": '1000' } },
                 "Launchers":
                   { "launcher1":
                       { "DBservers": ["Pavel"],
                         "Coordinators": ["Claus"] },
                     "launcher2":
                       { "DBservers": ["Perry"],
                         "Coordinators": ["Chantalle"] },
                     "launcher3":
                       { "DBservers": ["Pancho"],
                         "Coordinators": ["Chantalle"] } } } },
      { "action": "startLauncher", "dispatcher": "machine1", 
                                   "name": "launcher1" },
      { "action": "startLauncher", "dispatcher": "machine2",
                                   "name": "launcher2" },
      { "action": "startLauncher", "dispatcher": "machine3",
                                   "name": "launcher3" } ]
  };

var KickstarterLocalDefaults = {
  "agencyPrefix"            : "meier",
  "numberOfAgents"          : 3,
  "numberOfDBservers"       : 2,
  "startSecondaries"        : false,
  "numberOfCoordinators"    : 1,
  "DBserverIDs"             : ["Pavel", "Perry", "Pancho", "Paul", "Pierre",
                               "Pit", "Pia", "Pablo" ],
  "coordinatorIDs"          : ["Claus", "Chantalle", "Claire", "Claudia",
                               "Claas", "Clemens", "Chris" ],
  "dataPath"                : "",
  "logPath"                 : "",
  "agentExtPorts"           : [4001],
  "agentIntPorts"           : [7001],
  "DBserverPorts"           : [8629],
  "coordinatorPorts"        : [8530],
  "dispatchers"             : [["me", "me"]]
};
  
var KickstarterDistributedDefaults = {
  "agencyPrefix"            : "mueller",
  "numberOfAgents"          : 3,
  "numberOfDBservers"       : 3,
  "startSecondaries"        : false,
  "numberOfCoordinators"    : 3,
  "DBserverIDs"             : ["Pavel", "Perry", "Pancho", "Paul", "Pierre",
                               "Pit", "Pia", "Pablo" ],
  "coordinatorIDs"          : ["Claus", "Chantalle", "Claire", "Claudia",
                               "Claas", "Clemens", "Chris" ],
  "dataPath"                : "",
  "logPath"                 : "",
  "agentExtPorts"           : [4001],
  "agentIntPorts"           : [7001],
  "DBserverPorts"           : [8629],
  "coordinatorPorts"        : [8530],
  "dispatchers"             : [ [ "machine1", "tcp://machine1:8529"],
                                [ "machine2", "tcp://machine2:8529"],
                                [ "machine3", "tcp://machine3:8529"] ]
};

var _ = require("underscore");

function objmap (o, f) {
  var r = {};
  var k = _.keys(o);
  var i;
  for (i = 0;i < k.length; i++) {
    r[k[i]] = f(o[k[i]]);
  }
  return r;
}

function copy (o) {
  if (_.isArray(o)) {
    return _.map(o,copy);
  }
  if (_.isObject(o)) {
    return objmap(o,copy);
  }
  return o;
}

function PortFinder (list, dispatcherEndpoint) {
  if (!Array.isArray(list)) {
    throw "need a list as first argument";
  }
  if (typeof(dispatcherEndpoint) !== "string") {
    throw 'need an endpoint or "me" as second argument';
  }
  this.list = list;
  this.map = {};
  var i;
  for (i in this.list) {
    if (this.list.hasOwnProperty(i)) {
      this.map[this.list[i]] = true;
    }
  }
  this.dispatcherEndpoint = dispatcherEndpoint;
  this.pos = 0;
  this.port = 0;
}

PortFinder.prototype.next = function () {
  while (true) {   // will be left by return when port is found
    if (this.pos < this.list.length) {
      this.port = this.list[this.pos++];
    }
    else if (this.port !== 0) {
      this.port++;
      if (this.port > 65535) {
        this.port = 1024;
      }
      if (this.map.hasOwnProperty(this.port)) {
        continue;
      }
    }
    else {
      this.port = Math.floor(Math.random()*(65536-1024))+1024;
      if (this.map.hasOwnProperty(this.port)) {
        continue;
      }
    }
    // Check that port is available:
    if (this.dispatcherEndpoint !== "me" ||
        SYS_TEST_PORT("tcp://0.0.0.0:"+this.port)) {
      return this.port;
    }
  }
};
 
function exchangePort (endpoint, newport) {
  var pos = endpoint.lastIndexOf(":");
  if (pos < 0) {
    return endpoint+":"+newport;
  }
  return endpoint.substr(0,pos+1)+newport;
}

function fillConfigWithDefaults (config, defaultConfig) {
  var appendAttributes = {"DBserverIDs":true, "coordinatorIDs":true};
  var n;
  for (n in defaultConfig) {
    if (defaultConfig.hasOwnProperty(n)) {
      if (appendAttributes.hasOwnProperty(n)) {
        if (!config.hasOwnProperty(n)) {
          config[n] = copy(defaultConfig[n]);
        }
        else {
          config[n].concat(defaultConfig[n]);
        }
      }
      else {
        if (!config.hasOwnProperty(n)) {
          config[n] = copy(defaultConfig[n]);
        }
      }
    }
  }
}

function Kickstarter (userConfig, defaultConfig) {
  "use strict";
  if (defaultConfig === undefined) {
    defaultConfig = KickstarterLocalDefaults;
  }
  if (defaultConfig === "local") {
    defaultConfig = KickstarterLocalDefaults;
  }
  else if (defaultConfig === "distributed") {
    defaultConfig = KickstarterDistributedDefaults;
  }
  this.config = copy(userConfig);
  fillConfigWithDefaults(this.config, defaultConfig);
  this.commands = [];
  this.makePlan();
}

Kickstarter.prototype.makePlan = function() {
  // This sets up the plan for the cluster according to the options

  var config = this.config;
  var dispatchers = config.dispatchers;

  // If no dispatcher is there, configure a local one (ourselves):
  if (Object.keys(dispatchers).length === 0) {
    dispatchers.me = "me";
  }
  var nrDisp = Object.keys(dispatchers).length;

  var pf,pf2;   // lists of port finder objects
  var i;

  // Distribute agents to dispatchers (round robin, choosing ports)
  var d = 0;
  var agents = [];
  pf = [];   // will be filled lazily
  pf2 = [];  // will be filled lazily
  for (i = 0; i < config.numberOfAgents; i++) {
    // Find two ports:
    if (!pf.hasOwnProperty(d)) {
      pf[d] = new PortFinder(config.agentExtPorts, dispatchers[d][1]);
      pf2[d] = new PortFinder(config.agentIntPorts, dispatchers[d][1]);
    }
    agents.push({"dispatcher":dispatchers[d][0], 
                 "extPort":pf[d].next(),
                 "intPort":pf2[d].next()});
    if (++d >= dispatchers.length) {
      d = 0;
    }
  }

  // Distribute coordinators to dispatchers
  var coordinators = [];
  pf = [];
  d = 0;
  for (i = 0; i < config.numberOfCoordinators; i++) {
    if (!pf.hasOwnProperty(d)) {
      pf[d] = new PortFinder(config.coordinatorPorts, dispatchers[d][1]);
    }
    if (!config.coordinatorIDs.hasOwnProperty(i)) {
      config.coordinatorIDs[i] = "Coordinator"+i;
    }
    coordinators.push({"id":config.coordinatorIDs[i],
                       "dispatcher":dispatchers[d][0],
                       "port":pf[d].next()});
    if (++d >= dispatchers.length) {
      d = 0;
    }
  }

  // Distribute DBservers to dispatchers (secondaries if wanted)
  var DBservers = [];
  pf = [];
  d = 0;
  for (i = 0; i < config.numberOfDBservers; i++) {
    if (!pf.hasOwnProperty(d)) {
      pf[d] = new PortFinder(config.DBserverPorts, dispatchers[d][1]);
    }
    if (!config.DBserverIDs.hasOwnProperty(i)) {
      config.DBserverIDs[i] = "Primary"+i;
    }
    DBservers.push({"id":config.DBserverIDs[i],
                    "dispatcher":dispatchers[d][0],
                    "port":pf[d].next()});
    if (++d >= dispatchers.length) {
      d = 0;
    }
  }

  // Store this plan in object:
  this.coordinators = coordinators;
  this.DBservers = DBservers;
  this.agents = agents;
  this.dispatchers = {};
  var launchers = {};
  for (i = 0; i < dispatchers.length; i++) {
    this.dispatchers[dispatchers[i][0]] = dispatchers[i][1];
    launchers[dispatchers[i][0]] = { "DBservers": [], 
                                     "Coordinators": [],
                                     "Agents": [] };
  }

  // Register agents in launchers:
  for (i = 0; i < agents.length; i++) {
    launchers[agents[i].dispatcher].Agents.push({"extPort":agents[i].extPort,
                                                 "intPort":agents[i].intPort});
  }

  // Set up agency data:
  var agencyData = this.agencyData = {};
  var prefix = agencyData[config.agencyPrefix] = {};
  var tmp;

  // First the Target, we collect Launchers information at the same time:
  tmp = prefix.Target = {};
  tmp.Lock = '"UNLOCKED"';
  tmp.Version = '"1"';
  var dbs = tmp.DBServers = {};
  var map = tmp.MapIDToEndpoint = {};
  var s;
  for (i = 0; i < DBservers.length; i++) {
    s = DBservers[i];
    if (s.dispatcher === "me") {
      dbs[s.id] = map[s.id] 
                = '"'+exchangePort("tcp://127.0.0.1:0",s.port)+'"';
    }
    else {
      dbs[s.id] = map[s.id] 
                = '"'+exchangePort(this.dispatchers[s.dispatcher],s.port)+'"';
    }
    launchers[s.dispatcher].DBservers.push(s.id);
  }
  var coo = tmp.Coordinators = {};
  for (i = 0; i < coordinators.length; i++) {
    s = coordinators[i];
    if (s.dispatcher === "me") {
      coo[s.id] = map[s.id] 
                = '"'+exchangePort("tcp://127.0.0.1:0",s.port)+'"';
    }
    else {
      coo[s.id] = map[s.id] 
                = '"'+exchangePort(this.dispatchers[s.dispatcher],s.port)+'"';
    }
    launchers[s.dispatcher].Coordinators.push(s.id);
  }
  tmp.Databases = { "_system" : {} };
  tmp.Collections = { "_system" : {} };

  // Now Plan:
  prefix.Plan = copy(tmp);
  delete prefix.Plan.MapIDToEndpoint;

  // Now Current:
  prefix.Current = { "Lock"             : '"UNLOCKED"',
                     "Version"          : '"1"',
                     "DBservers"        : {},
                     "Coordinators"     : {},
                     "Databases"        : {"_system":{}},
                     "Collections"      : {"_system":{}},
                     "ServersRegistered": {"Version":'"1"'},
                     "ShardsCopied"     : {} };

  // Now Sync:
  prefix.Sync = { "ServerStates"       : {},
                  "Problems"           : {},
                  "LatestID"           : '"0"',
                  "Commands"           : {},
                  "HeartbeatIntervalMs": '1000' };
  tmp = prefix.Sync.Commands;
  for (i = 0; i < DBservers; i++) {
    tmp[DBservers[i].id] = '"SERVE"';
  }

  // Finally Launchers:
  prefix.Launchers = objmap(launchers, JSON.stringify);

  // make commands
  tmp = this.commands = [];
  var tmp2,j;
  for (i = 0; i < agents.length; i++) {
    tmp2 = { "action" : "startAgent", "dispatcher": agents[i].dispatcher,
             "ports": { "extPort": agents[i].extPort,
                        "intPort": agents[i].intPort }, 
             "peers": [] };
    for (j = 0; j < i; j++) {
      tmp2.peers.push( exchangePort( this.dispatchers[agents[j].dispatcher],
                                     agents[j].intPort ) );
    }
    tmp.push(tmp2);
  }
  tmp.push( { "action": "sendConfiguration", 
              "agency": exchangePort( this.dispatchers[agents[0].dispatcher],
                                      agents[0].extPort ),
              "data": agencyData } );
  for (i = 0; i < dispatchers.length; i++) {
    tmp.push( { "action": "startLauncher", "dispatcher": dispatchers[i][0],
                "name": dispatchers[i][0] } );
  }
};

Kickstarter.prototype.checkDispatchers = function() {
  // Checks that all dispatchers are active, if none is configured,
  // a local one is started.
};

Kickstarter.prototype.getStartupProgram = function() {
  // Computes the dispatcher commands and returns them as JSON
  return { "dispatchers": this.dispatchers,
           "commands": this.commands };
};

Kickstarter.prototype.launch = function() {
  // Starts the cluster according to startup plan
};

Kickstarter.prototype.shutdown = function() {
};

Kickstarter.prototype.isHealthy = function() {
};

exports.PortFinder = PortFinder;
exports.Kickstarter = Kickstarter;

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

/// Local Variables:
/// mode: outline-minor
/// outline-regexp: "/// @brief\\|/// @addtogroup\\|/// @page\\|// --SECTION--\\|/// @\\}\\|/\\*jslint"
/// End:
