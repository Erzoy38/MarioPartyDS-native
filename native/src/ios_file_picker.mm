// iOS: the system's document picker for the ROM (UIDocumentPickerViewController), since SDL 3.4 has
// no SDL_ShowOpenFileDialog there. The picked file arrives as a copy in the app's own temporary folder
// (asCopy), so no security-scoped access is needed; platform::keep_rom then keeps it in Documents.
#include "platform.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface MPDSRomPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) SDL_DialogFileCallback callback;
@property(nonatomic) void* userdata;
@end

// The picker holds its delegate weakly: this keeps it alive until the answer.
static MPDSRomPicker* g_picker = nil;

@implementation MPDSRomPicker

// Like SDL's dialogs: a list with the path, or an empty list when cancelled.
- (void)answer:(NSURL*)url {
    const SDL_DialogFileCallback callback = self.callback;
    void* const userdata = self.userdata;
    const char* list[2] = {url != nil ? url.fileSystemRepresentation : nullptr, nullptr};
    callback(userdata, list, -1);
    g_picker = nil;  // last: this may end the object
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    [self answer:urls.firstObject];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    (void)controller;
    [self answer:nil];
}

@end

namespace platform {

bool ios_show_rom_picker(SDL_Window* window, SDL_DialogFileCallback callback, void* userdata) {
    UIWindow* ui_window = (__bridge UIWindow*)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController* top = ui_window.rootViewController;
    while (top.presentedViewController != nil) top = top.presentedViewController;
    if (top == nil) return false;
    // Any file: .nds has no registered type, and the game checks the file itself.
    UIDocumentPickerViewController* picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    picker.allowsMultipleSelection = NO;
    g_picker = [MPDSRomPicker new];
    g_picker.callback = callback;
    g_picker.userdata = userdata;
    picker.delegate = g_picker;
    [top presentViewController:picker animated:YES completion:nil];
    return true;
}

}  // namespace platform
