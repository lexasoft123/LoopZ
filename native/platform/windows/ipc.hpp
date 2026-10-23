#pragma once
#include <loopz/document_json.hpp>
#include <string>
namespace loopz::windows {
std::wstring pipeName();
int runEngine();
int runBackground();
JSON engineRequest(const JSON& request);
void startEngine();
} // namespace loopz::windows
