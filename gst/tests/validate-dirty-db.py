#!/usr/bin/env python3

from testUtils import Utils
from Cluster import Cluster
from TestHelper import TestHelper

import random
import subprocess
import signal

###############################################################
# Test for validating the dirty db flag sticks repeated nodgst restart attempts
###############################################################


Print=Utils.Print
errorExit=Utils.errorExit

args = TestHelper.parse_args({"--keep-logs","--dump-error-details","-v","--leave-running","--clean-run"})
debug=args.v
pnodes=1
topo="mesh"
delay=1
chainSyncStrategyStr=Utils.SyncResyncTag
total_nodes = pnodes
killCount=1
killSignal=Utils.SigKillTag

killGstInstances= not args.leave_running
dumpErrorDetails=args.dump_error_details
keepLogs=args.keep_logs
killAll=args.clean_run

seed=1
Utils.Debug=debug
testSuccessful=False

def runNodgstAndGetOutput(myTimeout=3):
    """Startup nodgst, wait for timeout (before forced shutdown) and collect output. Stdout, stderr and return code are returned in a dictionary."""
    Print("Launching nodgst process.")
    cmd="programs/nodgst/nodgst --config-dir etc/gstio/node_bios --data-dir var/lib/node_bios --verbose-http-errors --http-validate-host=false"
    Print("cmd: %s" % (cmd))
    proc=subprocess.Popen(cmd.split(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if debug: Print("Nodgst process launched.")

    output={}
    try:
        if debug: Print("Setting nodgst process timeout.")
        outs,errs = proc.communicate(timeout=myTimeout)
        if debug: Print("Nodgst process has exited.")
        output["stdout"] = outs.decode("utf-8")
        output["stderr"] = errs.decode("utf-8")
        output["returncode"] = proc.returncode
    except (subprocess.TimeoutExpired) as _:
        Print("ERROR: Nodgst is running beyond the defined wait time. Hard killing nodgst instance.")
        proc.send_signal(signal.SIGKILL)
        return (False, None)

    if debug: Print("Returning success.")
    return (True, output)

random.seed(seed) # Use a fixed seed for repeatability.
cluster=Cluster(walletd=True)

try:
    TestHelper.printSystemInfo("BEGIN")

    cluster.setChainStrategy(chainSyncStrategyStr)

    cluster.killall(allInstances=killAll)
    cluster.cleanup()

    Print ("producing nodes: %d, topology: %s, delay between nodes launch(seconds): %d, chain sync strategy: %s" % (
        pnodes, topo, delay, chainSyncStrategyStr))

    Print("Stand up cluster")
    if cluster.launch(pnodes, total_nodes, topo=topo, delay=delay, dontBootstrap=True) is False:
        errorExit("Failed to stand up gst cluster.")

    node=cluster.getNode(0)

    Print("Kill cluster nodes.")
    cluster.killall(allInstances=killAll)

    Print("Restart nodgst repeatedly to ensure dirty database flag sticks.")
    timeout=6

    for i in range(1,4):
        Print("Attempt %d." % (i))
        ret = runNodgstAndGetOutput(timeout)
        assert(ret)
        assert(isinstance(ret, tuple))
        if not ret[0]:
            errorExit("Failed to startup nodgst successfully on try number %d" % (i))
        assert(ret[1])
        assert(isinstance(ret[1], dict))
        # pylint: disable=unsubscriptable-object
        stderr= ret[1]["stderr"]
        retCode=ret[1]["returncode"]
        expectedRetCode=2
        if retCode != expectedRetCode:
            errorExit("Expected return code to be %d, but instead received %d." % (expectedRetCode, retCode))
        db_dirty_msg="database dirty flag set"
        if db_dirty_msg not in stderr:
            errorExit("stderr should have contained \"%s\" but it did not. stderr=\n%s" % (db_dirty_msg, stderr))

    if debug: Print("Setting test result to success.")
    testSuccessful=True
finally:
    if debug: Print("Cleanup in finally block.")
    TestHelper.shutdown(cluster, None, testSuccessful, killGstInstances, False, keepLogs, killAll, dumpErrorDetails)

if debug: Print("Exiting test, exit value 0.")
exit(0)
