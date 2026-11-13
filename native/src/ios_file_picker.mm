// iOS: the system's document picker for the ROM (UIDocumentPickerViewController), since SDL 3.4 has
// no SDL_ShowOpenFileDialog there. The picked file arrives as a copy in the app's own temporary folder
// (asCopy), so no security-scoped access is needed; platform::keep_rom then keeps it in Documents.
// Each step goes to the log (MarioPartyDS.log in the Files app), and a picker that does not come up
// answers with an error after a few seconds, so the start screen never waits for nothing.
#include "platform.h"

#include <cstdio>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface MPDSRomPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) SDL_DialogFileCallback callback;
@property(nonatomic) void* userdata;
@property(nonatomic) BOOL shown;     // the picker is on screen
@property(nonatomic) BOOL answered;  // the callback has been called
@end

// The picker holds its delegate weakly: this keeps it alive until the answer.
static MPDSRomPicker* g_picker = nil;

static void log_line(const char* text) {
    std::printf("rom picker: %s\n", text);
    std::fflush(stdout);
}

@implementation MPDSRomPicker

// Like SDL's dialogs: a list with the path, an empty list when cancelled, null with SDL_SetError on
// an error. Only the first answer counts.
- (void)answer:(NSURL*)url error:(const char*)error {
    if (self.answered) return;
    self.answered = YES;
    const SDL_DialogFileCallback callback = self.callback;
    void* const userdata = self.userdata;
    if (error != nullptr) {
        log_line(error);
        SDL_SetError("%s", error);
        callback(userdata, nullptr, -1);
    } else {
        const char* list[2] = {url != nil ? url.fileSystemRepresentation : nullptr, nullptr};
        std::printf("rom picker: %s\n", list[0] != nullptr ? list[0] : "cancelled");
        std::fflush(stdout);
        callback(userdata, list, -1);
    }
    if (g_picker == self) g_picker = nil;  // last: this may end the object
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    [self answer:urls.firstObject error:nullptr];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    (void)controller;
    [self answer:nil error:nullptr];
}

@end

namespace {

// The window to show the picker over: SDL's, else the key window of the active scene.
UIWindow* game_window(SDL_Window* window) {
    UIWindow* ui_window = (__bridge UIWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (ui_window != nil && ui_window.rootViewController != nil) return ui_window;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) continue;
        for (UIWindow* candidate in ((UIWindowScene*)scene).windows) {
            if (candidate.isKeyWindow && candidate.rootViewController != nil) return candidate;
        }
    }
    return ui_window;
}

}  // namespace

namespace platform {

bool ios_show_rom_picker(SDL_Window* window, SDL_DialogFileCallback callback, void* userdata) {
    log_line([NSThread isMainThread] ? "asked (main thread)" : "asked (NOT the main thread)");
    UIWindow* ui_window = game_window(window);
    UIViewController* top = ui_window.rootViewController;
    while (top.presentedViewController != nil) top = top.presentedViewController;
    std::printf("rom picker: window %s, controller %s, its view %s\n", ui_window != nil ? "found" : "missing",
                top != nil ? NSStringFromClass([top class]).UTF8String : "missing",
                top.viewIfLoaded.window != nil ? "on screen" : "not on screen");
    std::fflush(stdout);
    if (top == nil) {
        SDL_SetError("no window to show the file picker over");
        return false;
    }
    // Any file: .nds has no registered type, and the game checks the file itself. public.item, not
    // public.data: in the Files app such files often have only the most general type, and with
    // public.data they were greyed out.
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem] asCopy:YES];
    picker.allowsMultipleSelection = NO;
    MPDSRomPicker* delegate = [MPDSRomPicker new];
    delegate.callback = callback;
    delegate.userdata = userdata;
    g_picker = delegate;
    picker.delegate = delegate;
    [top presentViewController:picker animated:YES completion:^{
        delegate.shown = YES;
        log_line("shown");
    }];
    // Not on screen after 3 seconds: give up, so the start screen takes input again.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (!delegate.shown && !delegate.answered) {
            [delegate answer:nil error:"the file picker did not come up (see MarioPartyDS.log)"];
        }
    });
    return true;
}

}  // namespace platform
