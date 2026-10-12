// Copyright 2026 scholay. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../core/control.hpp"
#include <string>
#include <stdexcept>

namespace rimes::windows::official {
inline constexpr const char* kAI = "builtin.openai-compatible";
inline constexpr const char* kCodex = "builtin.codex-cli";
inline constexpr const char* kTranslation = "builtin.apple-translation";
inline constexpr const char* kChord = "builtin.fly-chord-learning";
inline constexpr const char* kChordSchema = "my_combo";

// Native adapters belong to the plugin repository. The host validates package
// identity, integrity and grants before invoking any adapter.
inline void Validate(const core::Json& package) {
  const auto id = package.at("id").get<std::string>();
  const auto& contribution = package.at("contribution");
  const auto type = contribution.at("type").get<std::string>();
  const auto& options = contribution.at("options");
  if (id == kAI && type == "ai.channel.v1" &&
      options == core::Json{{"channel", "openai-compatible"}}) return;
  if (id == kCodex && type == "ai.channel.v1" &&
      options == core::Json{{"channel", "codex-cli"}}) return;
  if (id == kChord && type == "input.chord.v1" &&
      options == core::Json{{"schema", kChordSchema}, {"keymap", "Isaac2025"}}) return;
  if (id == kTranslation && type == "translation.v1" &&
      options == core::Json{{"source", "auto"}, {"target", "en"}}) {
    const auto text = contribution.at("instructions").at("default").get<std::string>();
    if (!text.empty() && text.size() <= 32768) return;
  }
  throw std::runtime_error("Unsupported Windows plugin contribution");
}
inline std::string Instruction(const core::Json& package, const std::string& language) {
  Validate(package);
  if (package.at("id") == kAI || package.at("id") == kCodex)
    return "Respond to the user's text. Return only the requested answer.";
  if (package.at("id") != kTranslation)
    throw std::runtime_error("This plugin has no AI action");
  auto instruction = package.at("contribution").at("instructions").at("default").get<std::string>();
  const std::string marker = "{targetLanguage}";
  for (std::size_t at = 0; (at = instruction.find(marker, at)) != std::string::npos; at += language.size())
    instruction.replace(at, marker.size(), language);
  return instruction;
}
}  // namespace rimes::windows::official
