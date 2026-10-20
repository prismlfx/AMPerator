#include "MicrophonePermission.h"

#include <juce_events/juce_events.h>

#import <AVFoundation/AVFoundation.h>

namespace amp
{
void requestMicrophoneAccess(std::function<void(bool)> done)
{
    auto deliver = [callback = std::move(done)](bool granted) mutable
    {
        juce::MessageManager::callAsync([cb = std::move(callback), granted] { cb(granted); });
    };

    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:
            deliver(true);
            return;
        case AVAuthorizationStatusDenied:
        case AVAuthorizationStatusRestricted:
            deliver(false);
            return;
        case AVAuthorizationStatusNotDetermined:
        default:
        {
            auto shared = std::make_shared<decltype(deliver)>(std::move(deliver));
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                     completionHandler:^(BOOL granted) { (*shared)(granted == YES); }];
            return;
        }
    }
}
} // namespace amp
