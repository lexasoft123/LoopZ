#include "desktop.hpp"
#import <Security/Security.h>

bool loopzLocalUIURL(NSURL* url) {
    if (!url)
        return false;
    auto* parts = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
    return
        [parts.scheme isEqual:@"loopz"] && [parts.host isEqual:@"app"] && !parts.user &&
        !parts.password && !parts.port && !parts.query && !parts.fragment &&
        [@[ @"/index.html", @"/loopz.js", @"/loopz.css" ] containsObject:parts.percentEncodedPath];
}

bool loopzValidateDesktopBundle() {
    SecCodeRef code = nullptr;
    SecStaticCodeRef file = nullptr;
    const auto self = SecCodeCopySelf(kSecCSDefaultFlags, &code);
    const auto copied =
        self == errSecSuccess ? SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &file) : self;
    const auto valid = copied == errSecSuccess
                           ? SecStaticCodeCheckValidity(file, kSecCSStrictValidate, nullptr)
                           : copied;
    if (file)
        CFRelease(file);
    if (code)
        CFRelease(code);
    return valid == errSecSuccess;
}

void loopzConfigureWebView(WKWebViewConfiguration* configuration) {
    configuration.preferences.javaScriptCanOpenWindowsAutomatically = NO;
    configuration.preferences.tabFocusesLinks = YES;
}

@implementation LZResources {
    NSURL* _root;
}
- (instancetype)initWithRoot:(NSURL*)root {
    if ((self = [super init]))
        _root = root;
    return self;
}
- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
    (void)webView;
    auto* request = task.request;
    if (!loopzLocalUIURL(request.URL) ||
        (request.HTTPMethod && ![request.HTTPMethod isEqual:@"GET"])) {
        [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                                   code:NSURLErrorUnsupportedURL
                                               userInfo:nil]];
        return;
    }
    auto* name = request.URL.lastPathComponent;
    auto* url = [_root URLByAppendingPathComponent:name isDirectory:NO];
    NSNumber* linked = nil;
    [url getResourceValue:&linked forKey:NSURLIsSymbolicLinkKey error:nil];
    NSData* data = linked.boolValue ? nil
                                    : [NSData dataWithContentsOfURL:url
                                                            options:NSDataReadingMappedIfSafe
                                                              error:nil];
    if (!data || data.length > 20 * 1024 * 1024) {
        [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                                   code:NSURLErrorFileDoesNotExist
                                               userInfo:nil]];
        return;
    }
    NSString* mime = [name isEqual:@"index.html"]  ? @"text/html"
                     : [name isEqual:@"loopz.css"] ? @"text/css"
                                                   : @"text/javascript";
    auto* response = [[NSURLResponse alloc] initWithURL:request.URL
                                               MIMEType:mime
                                  expectedContentLength:data.length
                                       textEncodingName:@"UTF-8"];
    [task didReceiveResponse:response];
    [task didReceiveData:data];
    [task didFinish];
}
- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
    // These small, sealed assets are served synchronously on the main thread.
    (void)webView;
    (void)task;
}
@end
