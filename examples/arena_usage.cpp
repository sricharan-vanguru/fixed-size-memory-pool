#include "memory_pool/monotonic_arena.hpp"

#include <iostream>
#include <string>
#include <utility>

struct SyntaxNode {
    SyntaxNode(std::string node_text,
               SyntaxNode* left_child = nullptr,
               SyntaxNode* right_child = nullptr)
        : text(std::move(node_text)), left(left_child), right(right_child) {}

    std::string text;
    SyntaxNode* left;
    SyntaxNode* right;
};

int main() {
    memory_pool::MonotonicArena compiler_pass_arena;

    SyntaxNode* const left = compiler_pass_arena.create<SyntaxNode>("4");
    SyntaxNode* const right = compiler_pass_arena.create<SyntaxNode>("5");
    SyntaxNode* const root =
        compiler_pass_arena.create<SyntaxNode>("+", left, right);

    std::cout << root->left->text << ' ' << root->text << ' '
              << root->right->text << '\n';
    std::cout << "chunks before reset=" << compiler_pass_arena.chunk_count()
              << '\n';

    // One reset destroys all three strings/nodes in reverse creation order and
    // makes their storage reusable for the next compiler pass.
    compiler_pass_arena.reset();
    std::cout << "used bytes after reset=" << compiler_pass_arena.bytes_used()
              << '\n';
}
