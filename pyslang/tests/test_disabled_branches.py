# SPDX-FileCopyrightText: Michael Popoloski
# SPDX-License-Identifier: MIT

"""Tests for ParserOptions.parseDisabledBranches / SyntaxTree.getParsedDisabledBranches."""

from pyslang import Bag, SourceManager
from pyslang.parsing import ParserOptions
from pyslang.syntax import SyntaxKind, SyntaxTree

# `the_mod` and `the_pkg` appear in both arms; USE_A is undefined, so the
# `ifdef arm is the not-taken (disabled) one.
SRC = (
    "`ifdef USE_A\n"
    "module the_mod (input a); the_pkg::t x; endmodule\n"
    "`else\n"
    "module the_mod (input b); endmodule\n"
    "`endif\n"
)


def _parse(parse_disabled: bool):
    opts = ParserOptions()
    opts.parseDisabledBranches = parse_disabled
    bag = Bag()
    bag.parserOptions = opts
    return SyntaxTree.fromFileInMemory(SRC, SourceManager(), "t", "t.sv", bag)


def test_option_exists():
    assert hasattr(ParserOptions(), "parseDisabledBranches")
    assert ParserOptions().parseDisabledBranches is False


def test_off_by_default():
    tree = SyntaxTree.fromText(SRC, "t.sv")
    assert len(tree.getParsedDisabledBranches()) == 0


def test_not_taken_branch_parsed_into_tree():
    tree = _parse(True)
    branches = tree.getParsedDisabledBranches()
    assert len(branches) == 1

    branch = branches[0]
    assert branch.directive.kind == SyntaxKind.IfDefDirective
    assert branch.tree.root.kind == SyntaxKind.ModuleDeclaration

    # The elided arm is a real, walkable tree: find identifiers inside it.
    idents = []

    def walk(node):
        if node.kind == SyntaxKind.IdentifierName:
            idents.append(node.identifier.valueText)
        for child in node:
            if child is not None and "TokenKind" not in str(getattr(child, "kind", "")):
                walk(child)

    walk(branch.tree.root)
    # `the_pkg` lives only in the not-taken arm; reachable only via this feature.
    assert "the_pkg" in idents


def test_disabled_tokens_carry_real_source_offsets():
    tree = _parse(True)
    branch = tree.getParsedDisabledBranches()[0]
    toks = branch.directive.disabledTokens
    assert len(toks) > 0
    # The first disabled token's offset indexes the original source: it's the
    # `module` keyword of the not-taken arm.
    start = toks[0].range.start.offset
    assert SRC.encode("utf-8")[start : start + len("module")] == b"module"
