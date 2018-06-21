"""
Test the lldb command line completion mechanism for the 'expr' command.
"""

from __future__ import print_function

import random
import os
import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbplatform
from lldbsuite.test import lldbutil

class CommandLineExprCompletionTestCase(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    NO_DEBUG_INFO_TESTCASE = True

    @expectedFailureAll(oslist=["windows"], bugnumber="llvm.org/pr24489")
    def test_expr_completion(self):
        self.build()
        self.main_source = "main.cpp"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)
        self.dbg.CreateTarget(self.getBuildArtifact("a.out"))

        # Try the completion before we have a context to complete on.
        self.assume_no_completions('expr some_expr')
        self.assume_no_completions('expr ')
        self.assume_no_completions('expr f')


        (target, process, thread, bkpt) = lldbutil.run_to_source_breakpoint(self,
                                          '// Break here', self.main_source_spec)

        # Completing member functions
        self.complete_from_to('expr some_expr.FooNoArgs',
                              'expr some_expr.FooNoArgsBar()')
        self.complete_from_to('expr some_expr.FooWithArgs',
                              'expr some_expr.FooWithArgsBar(')
        self.complete_from_to('expr some_expr.FooWithMultipleArgs',
                              'expr some_expr.FooWithMultipleArgsBar(')
        self.complete_from_to('expr some_expr.FooUnderscore',
                              'expr some_expr.FooUnderscoreBar_()')
        self.complete_from_to('expr some_expr.FooNumbers',
                              'expr some_expr.FooNumbersBar1()')
        self.complete_from_to('expr some_expr.StaticMemberMethod',
                              'expr some_expr.StaticMemberMethodBar()')

        # Completing static functions
        self.complete_from_to('expr Expr::StaticMemberMethod',
                              'expr Expr::StaticMemberMethodBar()')

        # Completing member variables
        self.complete_from_to('expr some_expr.MemberVariab',
                              'expr some_expr.MemberVariableBar')

        # Multiple completions
        self.completions_contain('expr some_expr.',
                                 ['some_expr.FooNumbersBar1()',
                                  'some_expr.FooUnderscoreBar_()',
                                  'some_expr.FooWithArgsBar(',
                                  'some_expr.MemberVariableBar'])

        self.completions_contain('expr some_expr.Foo',
                                 ['some_expr.FooNumbersBar1()',
                                  'some_expr.FooUnderscoreBar_()',
                                  'some_expr.FooWithArgsBar('])

        self.completions_contain('expr ',
                                 ['static_cast',
                                  'reinterpret_cast',
                                  'dynamic_cast'])

        self.completions_contain('expr 1 + ',
                                 ['static_cast',
                                  'reinterpret_cast',
                                  'dynamic_cast'])

        # Completion expr without spaces
        # This is a bit awkward looking for the user, but that's how
        # the completion API works at the moment.
        self.completions_contain('expr 1+',
                                 ['1+some_expr', "1+static_cast"])

        # Test with spaces
        self.complete_from_to('expr some_expr .FooNoArgs',
                              'expr some_expr .FooNoArgsBar()')
        self.complete_from_to('expr some_expr. FooNoArgs',
                              'expr some_expr. FooNoArgsBar()')
        self.complete_from_to('expr some_expr . FooNoArgs',
                              'expr some_expr . FooNoArgsBar()')
        self.complete_from_to('expr Expr :: StaticMemberMethod',
                              'expr Expr :: StaticMemberMethodBar()')
        self.complete_from_to('expr Expr ::StaticMemberMethod',
                              'expr Expr ::StaticMemberMethodBar()')
        self.complete_from_to('expr Expr:: StaticMemberMethod',
                              'expr Expr:: StaticMemberMethodBar()')

        # Addrof and deref
        self.complete_from_to('expr (*(&some_expr)).FooNoArgs',
                              'expr (*(&some_expr)).FooNoArgsBar()')
        self.complete_from_to('expr (*(&some_expr)) .FooNoArgs',
                              'expr (*(&some_expr)) .FooNoArgsBar()')
        self.complete_from_to('expr (* (&some_expr)) .FooNoArgs',
                              'expr (* (&some_expr)) .FooNoArgsBar()')
        self.complete_from_to('expr (* (& some_expr)) .FooNoArgs',
                              'expr (* (& some_expr)) .FooNoArgsBar()')

        # Addrof and deref (part 2)
        self.complete_from_to('expr (&some_expr)->FooNoArgs',
                              'expr (&some_expr)->FooNoArgsBar()')
        self.complete_from_to('expr (&some_expr) ->FooNoArgs',
                              'expr (&some_expr) ->FooNoArgsBar()')
        self.complete_from_to('expr (&some_expr) -> FooNoArgs',
                              'expr (&some_expr) -> FooNoArgsBar()')
        self.complete_from_to('expr (&some_expr)-> FooNoArgs',
                              'expr (&some_expr)-> FooNoArgsBar()')

        # Builtin arg
        self.complete_from_to('expr static_ca',
                              'expr static_cast')

        # Types
        self.complete_from_to('expr LongClassNa',
                              'expr LongClassName')
        self.complete_from_to('expr LongNamespaceName::NestedCla',
                              'expr LongNamespaceName::NestedClass')

        # Namespaces
        self.complete_from_to('expr LongNamespaceNa',
                              'expr LongNamespaceName::')

        # String
        self.complete_from_to('expr str.max_si',
                              'expr str.max_size()')
        self.complete_from_to('expr str.siz',
                              'expr str.size()')
        self.complete_from_to('expr (&str)->leng',
                              'expr (&str)->length()')

        # Multiple arguments
        self.complete_from_to('expr &some_expr + &some_e',
                              'expr &some_expr + &some_expr')
        self.complete_from_to('expr SomeLongVarNameWithCapitals + SomeLongVarName',
                              'expr SomeLongVarNameWithCapitals + SomeLongVarNameWithCapitals')
        self.complete_from_to('expr SomeIntVar + SomeIntV',
                              'expr SomeIntVar + SomeIntVar')

        # Multiple statements
        self.complete_from_to('expr long LocalVariable = 0; LocalVaria',
                              'expr long LocalVariable = 0; LocalVariable')

        # Custom Decls
        self.complete_from_to('expr auto l = [](int LeftHandSide, int bx){ return LeftHandS',
                              'expr auto l = [](int LeftHandSide, int bx){ return LeftHandSide')
        self.complete_from_to('expr struct LocalStruct { long MemberName; } ; LocalStruct S; S.Mem',
                              'expr struct LocalStruct { long MemberName; } ; LocalStruct S; S.MemberName')

        # Completing function call arguments
        self.complete_from_to('expr some_expr.FooWithArgsBar(some_exp',
                              'expr some_expr.FooWithArgsBar(some_expr')
        self.complete_from_to('expr some_expr.FooWithArgsBar(SomeIntV',
                              'expr some_expr.FooWithArgsBar(SomeIntVar')
        self.complete_from_to('expr some_expr.FooWithMultipleArgsBar(SomeIntVar, SomeIntVa',
                              'expr some_expr.FooWithMultipleArgsBar(SomeIntVar, SomeIntVar')

        # Function return values
        self.complete_from_to('expr some_expr.Self().FooNoArgs',
                              'expr some_expr.Self().FooNoArgsBar()')
        self.complete_from_to('expr some_expr.Self() .FooNoArgs',
                              'expr some_expr.Self() .FooNoArgsBar()')
        self.complete_from_to('expr some_expr.Self(). FooNoArgs',
                              'expr some_expr.Self(). FooNoArgsBar()')

    def generate_random_expr(self, run_index):
        """
        Generates a random expression. run_index seeds the rng, so
        the output of this method is always the same for the same run_index value
        """
        # Some random tokens we built our expression from.
        tokens = [".", ",", "(", ")", "{", "}", "foo", "a", "some_expr",
                  "->", "$", "&", " ", "::", "std", ":", "*", "+", "string",
                  "size", "\"", "'", "\\"]
        random.seed(run_index)
        num_tokens = random.randint(1, 8)
        result = ""
        for i in range(num_tokens):
            token = random.choice(tokens)
            result += token
        return result

    @expectedFailureAll(oslist=["windows"], bugnumber="llvm.org/pr24489")
    def test_stress_expr_completion(self):
        """
        We don't want the completion parsing to cause lldb to crash. This test just throws
        a few thousand (non-sensical) expressions at it to test this a bit.
        This test passes if lldb doesn't crash during the run.
        """
        self.build()
        self.main_source = "main.cpp"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)
        self.dbg.CreateTarget(self.getBuildArtifact("a.out"))

        (target, process, thread, bkpt) = lldbutil.run_to_source_breakpoint(self,
                                          '// Break here', self.main_source_spec)

        # 2000 seems enough to make this test about the same length as the other test cases
        # (which around 10s on my system).
        for i in range(2000):
            str_input = self.generate_random_expr(i)
            interp = self.dbg.GetCommandInterpreter()
            match_strings = lldb.SBStringList()
            num_matches = interp.HandleCompletion(str_input, len(str_input), 0, -1, match_strings)


    def assume_no_completions(self, str_input):
        interp = self.dbg.GetCommandInterpreter()
        match_strings = lldb.SBStringList()
        num_matches = interp.HandleCompletion(str_input, len(str_input), 0, -1, match_strings)

        available_completions = []
        for m in match_strings:
            available_completions.append(m)

        assert num_matches == 0, "Got matches, but didn't expect any: " + str(available_completions)

    def completions_contain(self, str_input, items):
        interp = self.dbg.GetCommandInterpreter()
        match_strings = lldb.SBStringList()
        num_matches = interp.HandleCompletion(str_input, len(str_input), 0, -1, match_strings)
        common_match = match_strings.GetStringAtIndex(0)

        for item in items:
            found = False
            for m in match_strings:
                if m == item:
                    found = True
            if not found:
                available_completions = []
                for m in match_strings:
                     available_completions.append(m)
                assert found, "Couldn't find completion " + item + " in completions " + str(available_completions)

    def complete_from_to(self, str_input, pattern):
        interp = self.dbg.GetCommandInterpreter()
        match_strings = lldb.SBStringList()
        num_matches = interp.HandleCompletion(str_input, len(str_input), 0, -1, match_strings)
        common_match = match_strings.GetStringAtIndex(0)

        if num_matches == 0:
            compare_string = str_input
        else:
            if common_match != None and len(common_match) > 0:
                compare_string = str_input + common_match
            else:
                compare_string = ""
                for idx in range(1, num_matches+1):
                    compare_string += match_strings.GetStringAtIndex(idx) + "\n"

        self.expect(
             compare_string, msg=COMPLETION_MSG(
             str_input, pattern, compare_string), exe=False, substrs=[pattern])
