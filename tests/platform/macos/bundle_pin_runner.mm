#import <Foundation/Foundation.h>
#include <dlfcn.h>

#include <array>
#include <cstdio>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2)
            return 2;
        std::array<char, 8192> output{};
        int status = 1;
        const char* stage = "dyld";
        const auto library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (library) {
            using Read = int (*)(char*, size_t);
            const auto read = reinterpret_cast<Read>(dlsym(library, "LoopZBundlePinFixture"));
            if (!read)
                return 2;
            stage = "loader";
            status = read(output.data(), output.size());
        } else {
            std::snprintf(output.data(), output.size(), "%s", dlerror());
        }
        NSDictionary* result =
            @{@"status" : @(status),
              @"value" : @(output.data()),
              @"stage" : @(stage)};
        const auto data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
        if (!data)
            return 2;
        std::fwrite(data.bytes, 1, data.length, stdout);
        std::putchar('\n');
        return 0;
    }
}
