#include <loopz/macos/editor_protocol.hpp>
#include <loopz/macos/route_store.hpp>
#import <Foundation/Foundation.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
std::string json(id value) {
    NSData* data = [NSJSONSerialization dataWithJSONObject:value options:0 error:nil];
    if (!data)
        throw std::invalid_argument("Invalid fixture JSON");
    return {static_cast<const char*>(data.bytes), data.length};
}
} // namespace
// Offline browser-test adapter. It decodes the production wire contract and
// applies the production reducer; it has no connection, file store or audio IO.
int main() {
    @autoreleasepool {
        try {
            std::string input;
            std::array<char, 16384> buffer{};
            while (std::cin) {
                std::cin.read(buffer.data(), buffer.size());
                input.append(buffer.data(), static_cast<size_t>(std::cin.gcount()));
                if (input.size() > 4 * 1024 * 1024)
                    throw std::invalid_argument("Fixture input exceeds limit");
            }
            id value = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:input.data()
                                                                              length:input.size()]
                                                       options:0
                                                         error:nil];
            if (![value isKindOfClass:NSDictionary.class] || [value count] != 2 ||
                ![value[@"document"] isKindOfClass:NSDictionary.class] ||
                ![value[@"request"] isKindOfClass:NSDictionary.class])
                throw std::invalid_argument("Fixture requires document and request objects");
            const auto document = loopz::macos::decodeRouteDocumentJSON(json(value[@"document"]));
            const auto request = loopz::macos::decodeEditorRequest(json(value[@"request"]));
            const auto next = loopz::edit::applyCommands(document, request.commands);
            std::cout << loopz::macos::encodeRouteDocumentJSON(next) << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 2;
        }
    }
}
