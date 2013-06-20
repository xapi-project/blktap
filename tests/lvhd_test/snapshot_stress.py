#!/usr/bin/python

import sys
import random
import pickle

sys.path.append("/opt/xensource/sm")
import cleanup

class VDIAction:
    """Base class of all VDI actions"""

    def __init__(self, context):
        pass

    def applicable(self, context):
        """Decides during program generation whether the given action should
        be applicable within the given @context or not."""
        raise NotImplementedError
    applicable = classmethod(applicable)
    
    def execute(self, context):
        """Execute the action in @context."""
        raise NotImplementedError
    execute = classmethod(execute)

    def sum(self, n):
        """Return the new number of nodes after being executed on @n nodes"""
        raise NotImplementedError
    sum = classmethod(sum)

class SnapshotAction(VDIAction):
    def __init__(self, context):
        self.node = context.randomNode()
        
    def applicable(self, context):
        # During the last steps of the program, we'd rather clean up,
        # reducing the genealogy back to a single node. Ensure that we
        # never create more nodes then there are steps left.
        allowance = context.num_actions - context.i

        # Last run should rather do a coalesce. Increase the holdoff.
        allowance -= 1

        return allowance > context.nodeCount()
    applicable = classmethod(applicable)

    def execute(self, context):
        vdi = context.getNode(self.node)
        snapshot = self.snapshotVDI(vdi)
        context.addVDI(snapshot)

    def snapshotVDI(self, vdi):
        raise NotImplementedError()

    sum = classmethod(sum)

    def __str__(self):
        """Returns '<__name__>(<node>)' for pretty printing"""
        return "%s(%d)" % (self.__class__.__name__, self.node)

class RemoveAction(VDIAction):

    def __init__(self, context):
        self.node = context.randomNode()

    def applicable(self, context):
        # A vdi-destroy should not be applied on the last node in a
        # genealogy. Otherwise there'd be nothing left to continue
        # from.
        return context.nodeCount() > 1
    applicable = classmethod(applicable)

    def execute(self, context):
        vdi = context.getNode(self.node)
        self.destroyVDI(vdi)
        context.delNode(self.node)

    def destroyVDI(self, vdi):
        raise NotImplementedError()

    def sum(self, count):
        return count - 1
    sum = classmethod(sum)

    def __str__(self):
        """Returns '<__name__>(<node>)' for pretty printing"""
        return "%s(%d)" % (self.__class__.__name__, self.node)

class CoalesceAction(VDIAction):
    def applicable(self, context):
        if context.nodeCount() == 1:
            return True
        if not context.program.SNAPSHOT.applicable(context):
            return False
        return True
    applicable = classmethod(applicable)

    def execute(self, context):
        if context.nodeCount() == 0:
            return
        if not context.sr:
            vdi = context.getNode(0)
            context.sr = self.resolveSR(vdi)
        self.coalesceSR(context.sr)

    def resolveSR(self, vdi): # abstract
        return None

    def coalesceSR(self): # abstract
        return

    def sum(self, n):
        return n
    sum = classmethod(sum)

    def __str__(self):
        """Returns '<__name__>' for pretty printing"""
        return self.__class__.__name__

class TestingProgram:
    """A TestingProgram is a random sequence of
       snapshot/remove/coalesce/.. actions, executed on a single
       family tree of related VDIs."""

    ACTION_CLASSES = []

    def __init__(self, num_actions):
        self.actions = []
        self.num_actions = num_actions

    class Context:
        sr = None

        #
        # Basic program execution context, for actions to decide
        # whether they're currently applicable or not.
        #
        def __init__(self, program, nodes):
            self.program = program
            self.i = 0
            self.num_actions = program.num_actions
            self._vdis = nodes

            self.random = random.Random()

        def addVDI(self, vdi):
            self._vdis.append(vdi)
            
        def delNode(self, idx):
            del self._vdis[idx]

        def getNode(self, idx):
            return self._vdis[idx]

        def nodeCount(self):
            return len(self._vdis)

        def randomNode(self):
            return random.randint(0, self.nodeCount()-1)
        
        def description(self):
            return "%d node(s), iteration %d < %d" % (self.nodeCount(), self.i, self.num_actions)

    def randomAction(self, context):
        """"Generate a random action executable in given @context."""

        total = sum(map(lambda x: x[1], self.ACTION_CLASSES))
        
        # build a multiset of candidates for the next action
        next = []
        for action, probability in self.ACTION_CLASSES:
            if action.applicable(context):
                # number of seats is given by probability. up to 10 items total.
                n_items = int(10 * probability / total)
                next += [action] * n_items

        # choose a random action
        try:
            action = context.random.choice(next)
        except IndexError:
            raise Exception("Dead end - no applicable actions to continue\n" + 
                            context.description())

        # instanciate the action
        return action(context)
    randomAction = classmethod(randomAction)

    def randomProgram(self, num_actions):
        """"Generate a random TestingProgram of given @num_actions. 

        The generator assumes only a single VDI as the starting point.

        The last actions in the resulting program attempt to reduce
        the snapshot tree to its original length. Note that any unary
        @num_actions parameter passed can only generate a script generating
        a residual of 2 VDIs."""

        raise NotImplementedError()

    randomProgram = classmethod(randomProgram)

    def execute(self, action, context):

        # execute action
        print >> sys.stderr,  context.i, action,
        action.execute(context)
        print >> sys.stderr, "# %d" % context.nodeCount()

        # perform self checks, if available
        self.check(action)


    def run(self, num_actions, nodes):

        context = TestingProgram.Context(self, nodes)

        for context.i in range(num_actions):

            action = self.randomAction(context)

            self.actions.append(action)

            self.execute(action, context)


    def rerun(self, nodes):

        context = TestingProgram.Context(self, nodes)

        for action in self.actions:

            self.execute(action, context)

    def check(self, action):
        """Self-check performed after each @action executed. To be implemented
        by subclasses."""
        pass

    def save(self, path):
        """Save @self to @path"""
        file = open(path, "w")
        pickle.dump(self, file)

    def load(self, path):
        """Restore program saved in @path"""
        file = open(path, "r")
        return pickle.load(file)
    load = classmethod(load)

#     def run(self, path, vdi):
#         """Restore program saved in @path, then execute it on @vdi."""
#         prog = TestingProgram.restore(path)
#         prog.execute(vdi)
#         return prog
#     run = classmethod(run)

if __name__ == "__main__":
    import sys, os, re

    PATH = "/tmp/snapshot"

    def usage():
        print "Usage:"
        print "%s gen <num_actions> <vdi_uuid>" % sys.argv[0]
        print "%s rerun <path> <vdi_uuid>" % sys.argv[0]

    def xe(cmd, *args):
        argv = [ "/opt/xensource/bin/xe", cmd ]
        argv += args
        stdin, stdout = os.popen4(argv)
        return stdout.readlines()

    class XeSnapshotVDI(SnapshotAction):
        def snapshotVDI(self, vdi):
            output = xe("vdi-snapshot", "uuid=%s" % vdi)
            m = None
            try:
                uuid = output[0].strip()
                m = re.match("^[-a-f0-9]{36}$", uuid)
            finally:
                if not m:
                    raise Exception("Failure creating VDI snapshot:\n" +\
                            str(output))

            return uuid
        snapshotVDI = classmethod(snapshotVDI)

    class XeRemoveVDI(RemoveAction):
        def destroyVDI(self, vdi):
            output = xe("vdi-destroy", "uuid=%s" % vdi)
            if output:
                raise Exception("Failure destroying VDI:\n" + str(output))
        destroyVDI = classmethod(destroyVDI)

    class XeCoalesce(CoalesceAction):
        def resolveSR(self, vdi):
            output = xe("vdi-list", "uuid=%s" % vdi, "params=sr-uuid",
                    "--minimal")
            uuid = output[0].strip()
            return uuid
        resolveSR = classmethod(resolveSR)

        def coalesceSR(self, sr):
            cleanup.run(sr, "lvhd", None, True, False, True, False)
        coalesceSR = classmethod(coalesceSR)

    class XeTestingProgram(TestingProgram):
        """The resulting program class"""
        SNAPSHOT = XeSnapshotVDI
        REMOVE =   XeRemoveVDI
        COALESCE = XeCoalesce
        ACTION_CLASSES = [ (SNAPSHOT, .5), (REMOVE, .4), (COALESCE, .1) ]

    if len(sys.argv) != 4:
        usage()
        sys.exit(1)

    cmd = sys.argv[1]
    if cmd == "gen":
        num_actions = int(sys.argv[2])
        vdi = sys.argv[3]
        prog = XeTestingProgram(num_actions)
        prog.run(num_actions, [vdi])
    elif cmd == "rerun":
        path = sys.argv[2]
        vdi = sys.argv[3]
        prog = XeTestingProgram.load(path)
        prog.rerun([vdi])
    else:
        usage()
