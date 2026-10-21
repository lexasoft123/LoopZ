#pragma once
#import <Foundation/Foundation.h>
#include <filesystem>
#include <string>
#include <sys/types.h>

namespace loopz::macos {
std::string audioValidationDocument(bool publish);
void verifyAudioPayload(const std::filesystem::path& app);
NSDictionary* maintainAudioComponents(const std::filesystem::path& app,
                                      bool allowAudioActivation = false, uid_t expectedUser = 0);
} // namespace loopz::macos
