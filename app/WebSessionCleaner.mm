#include "WebSessionCleaner.h"

#include <juce_gui_extra/juce_gui_extra.h>

#import <WebKit/WebKit.h>

namespace amp
{
// JUCE's WKWebView configuration is a plain [WKWebViewConfiguration new], i.e. the shared default (persistent)
// WKWebsiteDataStore, which is the one cleared here.
void clearWebSessionFor(const juce::String& domainSuffix, std::function<void()> done)
{
    juce::WebBrowserComponent::clearCookies(); // NSHTTPCookieStorage only; WKWebView keeps its own

    NSString* suffix = [NSString stringWithUTF8String:domainSuffix.toRawUTF8()];
    WKWebsiteDataStore* store = [WKWebsiteDataStore defaultDataStore];
    NSSet<NSString*>* types = [WKWebsiteDataStore allWebsiteDataTypes];
    auto finished = std::make_shared<std::function<void()>>(std::move(done));
    auto finish = [finished]
    {
        juce::MessageManager::callAsync([finished] { if (*finished) (*finished)(); });
    };

    [store fetchDataRecordsOfTypes:types
                 completionHandler:^(NSArray<WKWebsiteDataRecord*>* records)
    {
        NSMutableArray<WKWebsiteDataRecord*>* matching = [NSMutableArray array];
        for (WKWebsiteDataRecord* record in records)
            if ([record.displayName isEqualToString:suffix] || [record.displayName hasSuffix:[@"." stringByAppendingString:suffix]])
                [matching addObject:record];

        if ([matching count] == 0)
        {
            finish();
            return;
        }
        [store removeDataOfTypes:types forDataRecords:matching completionHandler:^{ finish(); }];
    }];
}
} // namespace amp
