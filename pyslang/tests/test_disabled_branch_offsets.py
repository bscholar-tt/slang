# SPDX-FileCopyrightText: Michael Popoloski
# SPDX-License-Identifier: MIT

"""Locks the offset contract for parsed disabled branches.

A recovered branch tree is parsed from a slice of the original source. Its
node offsets are 0-based into that slice, so a consumer maps a branch-local
offset ``o`` back to the file as ``anchor + o``. The anchor a consumer can
observe is ``directive.disabledTokens[0].range.start.offset``. This test pins
the invariant that the slice begins at exactly that anchor -- otherwise every
translated offset is shifted by the leading-trivia length and renames mis-land.

It also documents that leading trivia (comments/whitespace before the first
disabled token) is intentionally NOT part of the recovered branch text: that's
harmless for renaming (comments are never targets) but means the slice is "from
the first disabled token", not the byte-exact branch region.
"""

import gc

from pyslang import Bag, SourceManager
from pyslang.parsing import ParserOptions
from pyslang.syntax import SyntaxKind, SyntaxPrinter, SyntaxTree

SRC = (
    "`ifdef NOPE\n"
    "  // leading comment mentioning old_pkg\n"
    "  old_pkg::t x;\n"
    "`endif\n"
    "module live; endmodule\n"
)


def _branch():
    opts = ParserOptions()
    opts.parseDisabledBranches = True
    bag = Bag()
    bag.parserOptions = opts
    tree = SyntaxTree.fromFileInMemory(SRC, SourceManager(), "t", "t.sv", bag)
    branches = tree.getParsedDisabledBranches()
    assert len(branches) == 1
    # The parent `tree` deliberately goes out of scope here: the returned branch
    # must keep it alive on its own (branch.directive points into it).
    return branches[0]


def test_slice_anchor_equals_first_disabled_token_start():
    """The branch slice begins at disabledTokens[0].start; a branch-local offset
    plus that anchor must map back to the same token in the original source."""
    branch = _branch()
    anchor = branch.directive.disabledTokens[0].range.start.offset

    # branch-local offset of the renameable identifier inside the subtree
    def find(node):
        if (
            node.kind == SyntaxKind.IdentifierName
            and node.identifier.valueText == "old_pkg"
        ):
            return node.identifier.range.start.offset
        for child in node:
            if child is not None and "TokenKind" not in str(
                getattr(child, "kind", "")
            ):
                got = find(child)
                if got is not None:
                    return got
        return None

    local = find(branch.tree.root)
    assert local is not None
    # The contract: anchor + local lands exactly on the real old_pkg token.
    assert SRC[anchor + local : anchor + local + len("old_pkg")] == "old_pkg"


def test_leading_comment_is_not_in_recovered_text():
    """Documents (not endorses) that leading trivia is dropped from the slice.
    Harmless for rename (comments aren't targets); flags that 'exact branch
    text' is really 'from the first disabled token'."""
    branch = _branch()
    reprint = SyntaxPrinter.printFile(branch.tree)
    assert "old_pkg::t x;" in reprint
    assert "leading comment" not in reprint


def test_directive_survives_dropped_parent_tree():
    """branch.directive is a raw pointer into the parent tree; holding only the
    branch must keep that parent alive (regression for a use-after-free)."""
    branch = _branch()  # parent tree already out of scope
    gc.collect()
    # On a UAF these reads return garbage / raise UnicodeDecodeError.
    assert branch.directive.kind == SyntaxKind.IfDefDirective
    tok = branch.directive.disabledTokens[0]
    assert tok.valueText == "old_pkg"
    off = tok.range.start.offset
    assert SRC[off : off + len("old_pkg")] == "old_pkg"
