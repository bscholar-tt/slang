//------------------------------------------------------------------------------
// SyntaxTree.cpp
// Top-level parser interface
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/syntax/SyntaxTree.h"

#include "slang/parsing/Parser.h"
#include "slang/parsing/ParserMetadata.h"
#include "slang/parsing/Preprocessor.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxPrinter.h"
#include "slang/text/SourceManager.h"
#include "slang/util/TimeTrace.h"

namespace slang::syntax {

using namespace parsing;

SyntaxTree::SyntaxTree(SyntaxNode* root, SourceManager& sourceManager, BumpAllocator&& alloc,
                       const SourceLibrary* library,
                       const std::shared_ptr<SyntaxTree>& preTransform) :
    rootNode(root), library(library), sourceMan(sourceManager), alloc(std::move(alloc)),
    metadata(ParserMetadata::fromSyntax(*root)) {
    if (preTransform && !metadata.eofToken)
        metadata.eofToken = preTransform->getMetadata().eofToken.deepClone(this->alloc);
}

SyntaxTree::~SyntaxTree() = default;

SyntaxTree::TreeOrError SyntaxTree::fromFile(std::string_view path) {
    return fromFile(path, getDefaultSourceManager());
}

SyntaxTree::TreeOrError SyntaxTree::fromFile(std::string_view path, SourceManager& sourceManager,
                                             const Bag& options) {
    auto buffer = sourceManager.readSource(path);
    if (!buffer)
        return nonstd::make_unexpected(std::pair{buffer.error(), path});
    return create(sourceManager, std::span(&buffer.value(), 1), options, {}, false);
}

SyntaxTree::TreeOrError SyntaxTree::fromFiles(std::span<const std::string_view> paths) {
    return fromFiles(paths, getDefaultSourceManager());
}

SyntaxTree::TreeOrError SyntaxTree::fromFiles(std::span<const std::string_view> paths,
                                              SourceManager& sourceManager, const Bag& options) {
    SmallVector<SourceBuffer, 4> buffers(paths.size(), UninitializedTag());
    for (auto path : paths) {
        auto buffer = sourceManager.readSource(path);
        if (!buffer)
            return nonstd::make_unexpected(std::pair{buffer.error(), path});

        buffers.push_back(*buffer);
    }

    return create(sourceManager, buffers, options, {}, false);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromText(std::string_view text, std::string_view name,
                                                 std::string_view path) {
    return fromText(text, getDefaultSourceManager(), name, path);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromText(std::string_view text, const Bag& options,
                                                 std::string_view name, std::string_view path) {
    return fromText(text, getDefaultSourceManager(), name, path, options);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromText(std::string_view text,
                                                 SourceManager& sourceManager,
                                                 std::string_view name, std::string_view path,
                                                 const Bag& options, const SourceLibrary* library) {
    SourceBuffer buffer = sourceManager.assignText(path, text, {}, library);
    if (!buffer)
        return nullptr;

    if (!name.empty())
        sourceManager.addLineDirective(SourceLocation(buffer.id, 0), 2, name, 0);

    return create(sourceManager, std::span(&buffer, 1), options, {}, true);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromFileInMemory(std::string_view text,
                                                         SourceManager& sourceManager,
                                                         std::string_view name,
                                                         std::string_view path,
                                                         const Bag& options) {
    SourceBuffer buffer = sourceManager.assignText(path, text);
    if (!buffer)
        return nullptr;

    if (!name.empty())
        sourceManager.addLineDirective(SourceLocation(buffer.id, 0), 2, name, 0);

    return create(sourceManager, std::span(&buffer, 1), options, {}, false);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromBuffer(const SourceBuffer& buffer,
                                                   SourceManager& sourceManager, const Bag& options,
                                                   MacroList inheritedMacros) {
    return create(sourceManager, std::span(&buffer, 1), options, inheritedMacros, false);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromBuffers(std::span<const SourceBuffer> buffers,
                                                    SourceManager& sourceManager,
                                                    const Bag& options, MacroList inheritedMacros) {
    return create(sourceManager, buffers, options, inheritedMacros, false);
}

SourceManager& SyntaxTree::getDefaultSourceManager() {
    static SourceManager instance;
    return instance;
}

SyntaxTree::SyntaxTree(SyntaxNode* root, const SourceLibrary* library, SourceManager& sourceManager,
                       BumpAllocator&& alloc, Diagnostics&& diagnostics, ParserMetadata&& metadata,
                       PreprocessorMetadata&& preprocessorMetadata, Bag options) :
    rootNode(root), library(library), sourceMan(sourceManager), alloc(std::move(alloc)),
    diagnosticsBuffer(std::move(diagnostics)), options_(std::move(options)),
    metadata(std::move(metadata)), preprocessorMetadata(std::move(preprocessorMetadata)) {
}

std::shared_ptr<SyntaxTree> SyntaxTree::create(SourceManager& sourceManager,
                                               std::span<const SourceBuffer> sources,
                                               const Bag& options, MacroList inheritedMacros,
                                               bool guess) {
    if (sources.empty())
        SLANG_THROW(std::invalid_argument("sources cannot be empty"));

    TimeTraceScope timeScope("parseFile"sv, [&] {
        if (sources.size() == 1)
            return std::string(sourceManager.getRawFileName(sources[0].id));
        else
            return "<multi-buffer>"s;
    });

    BumpAllocator alloc;
    Diagnostics diagnostics;
    Preprocessor preprocessor(sourceManager, alloc, diagnostics, options, inheritedMacros);

    const SourceLibrary* library = nullptr;
    for (auto it = sources.rbegin(); it != sources.rend(); it++) {
        preprocessor.pushSource(*it);

        if (it != sources.rbegin() && library != it->library) {
            SLANG_THROW(std::invalid_argument("All sources provided to a single SyntaxTree must be "
                                              "from the same source library"));
        }

        library = it->library;
    }

    const auto ppOpts = options.get<PreprocessorOptions>();
    if (ppOpts && ppOpts->bufferChangeCB)
        ppOpts->bufferChangeCB(sources.front().id, false, false);

    const auto parserOptions = options.getOrDefault<ParserOptions>();

    Parser parser(preprocessor, options);

    SyntaxNode* root;
    if (!guess)
        root = &parser.parseCompilationUnit();
    else {
        root = &parser.parseGuess();
        if (!parser.isDone())
            return create(sourceManager, sources, options, inheritedMacros, false);
    }

    auto tree = std::shared_ptr<SyntaxTree>(
        new SyntaxTree(root, library, sourceManager, std::move(alloc), std::move(diagnostics),
                       parser.getMetadata(), preprocessor.getMetadata(), options));

    // Opt-in: parse the not-taken conditional branches into standalone trees.
    // Skipped for `guess` sub-parses (snippets) to keep the pass bounded.
    if (!guess && parserOptions.parseDisabledBranches)
        tree->parseDisabledBranchTrees(options);

    return tree;
}

void SyntaxTree::parseDisabledBranchTrees(const Bag& options) {
    // Parse each disabled branch as an isolated snippet. Clear the flag for the
    // sub-parse so a nested conditional inside a disabled branch doesn't trigger
    // unbounded recursion; one level of recovery is enough for tooling and keeps
    // behavior predictable.
    Bag subOptions = options;
    auto subParserOpts = options.getOrDefault<ParserOptions>();
    subParserOpts.parseDisabledBranches = false;
    subOptions.set(subParserOpts);

    auto handleDirective = [&](const SyntaxNode* dir) {
        const TokenList* disabled = nullptr;
        switch (dir->kind) {
            case SyntaxKind::IfDefDirective:
            case SyntaxKind::IfNDefDirective:
            case SyntaxKind::ElsIfDirective:
                disabled = &dir->as<ConditionalBranchDirectiveSyntax>().disabledTokens;
                break;
            case SyntaxKind::ElseDirective:
            case SyntaxKind::EndIfDirective:
                disabled = &dir->as<UnconditionalBranchDirectiveSyntax>().disabledTokens;
                break;
            default:
                return;
        }

        if (!disabled || disabled->empty())
            return;

        // The disabled tokens are real lexed tokens with genuine source offsets,
        // so we can recover the exact branch text by slicing the source buffer
        // between the first and last token.
        Token firstTok = (*disabled)[0];
        Token lastTok = (*disabled)[disabled->size() - 1];
        SourceLocation startLoc = firstTok.location();
        SourceLocation endLoc = lastTok.range().end();
        if (!startLoc.valid() || !endLoc.valid() || startLoc.buffer() != endLoc.buffer())
            return;

        std::string_view fullText = sourceMan.getSourceText(startLoc.buffer());
        size_t s = startLoc.offset();
        size_t e = endLoc.offset();
        if (s > e || e > fullText.size())
            return;

        std::string_view branchText = fullText.substr(s, e - s);
        auto subTree = SyntaxTree::fromText(branchText, sourceMan, "disabled-branch", "",
                                            subOptions, library);
        if (subTree)
            disabledBranches.push_back({dir, std::move(subTree)});
    };

    // Conditional directives live in token trivia, not as ordinary children, so
    // walk every token in the tree and inspect its trivia.
    std::vector<const SyntaxNode*> stack{rootNode};
    while (!stack.empty()) {
        const SyntaxNode* node = stack.back();
        stack.pop_back();
        if (!node)
            continue;

        size_t count = node->getChildCount();
        for (size_t i = 0; i < count; i++) {
            if (const SyntaxNode* childNode = node->childNode(i)) {
                stack.push_back(childNode);
            }
            else if (Token token = node->childToken(i)) {
                for (const auto& tr : token.trivia()) {
                    if (tr.kind == parsing::TriviaKind::Directive) {
                        if (const SyntaxNode* dir = tr.syntax())
                            handleDirective(dir);
                    }
                }
            }
        }
    }
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromLibraryMapFile(std::string_view path,
                                                           SourceManager& sourceManager,
                                                           const Bag& options) {
    auto buffer = sourceManager.readSource(path);
    if (!buffer)
        return nullptr;

    return fromLibraryMapBuffer(*buffer, sourceManager, options);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromLibraryMapText(std::string_view text,
                                                           SourceManager& sourceManager,
                                                           std::string_view name,
                                                           std::string_view path,
                                                           const Bag& options) {
    SourceBuffer buffer = sourceManager.assignText(path, text);
    if (!buffer)
        return nullptr;

    if (!name.empty())
        sourceManager.addLineDirective(SourceLocation(buffer.id, 0), 2, name, 0);

    return fromLibraryMapBuffer(buffer, sourceManager, options);
}

std::shared_ptr<SyntaxTree> SyntaxTree::fromLibraryMapBuffer(const SourceBuffer& buffer,
                                                             SourceManager& sourceManager,
                                                             const Bag& options) {
    sourceManager.setBufferKind(buffer.id, SourceManager::BufferKind::LibraryMap);

    BumpAllocator alloc;
    Diagnostics diagnostics;
    Preprocessor preprocessor(sourceManager, alloc, diagnostics, options);
    preprocessor.pushSource(buffer);

    Parser parser(preprocessor, options);
    auto& root = parser.parseLibraryMap();

    return std::shared_ptr<SyntaxTree>(
        new SyntaxTree(&root, nullptr, sourceManager, std::move(alloc), std::move(diagnostics),
                       parser.getMetadata(), preprocessor.getMetadata(), options));
}

bool SyntaxTree::validate() const {
    auto text = SyntaxPrinter(sourceManager())
                    .setIncludeDirectives(true)
                    .setExpandIncludes(true)
                    .setExpandMacros(true)
                    .print(*this)
                    .str();

    SourceManager tempManager;
    auto buf = tempManager.assignText(text);

    BumpAllocator tempAlloc;
    Diagnostics tempDiags;
    Preprocessor preprocessor(tempManager, tempAlloc, tempDiags, options_);
    preprocessor.pushSource(buf);

    Parser parser(preprocessor, options_);

    SyntaxNode* newRoot;
    if (rootNode->kind == SyntaxKind::CompilationUnit)
        newRoot = &parser.parseCompilationUnit();
    else
        newRoot = &parser.parseGuess();

    return newRoot->isEquivalentTo(*rootNode);
}

} // namespace slang::syntax
