#import "IKViewFinder.h"
#import <AppKit/AppKit.h>
#include "finderhandoff.h"
#import <objc/runtime.h>

@interface IKViewFinder (NativeTestAction)
- (void)openSelection:(NSMenuItem *)sender;
@end

// This mock is installed only inside this disposable test process. No real
// NSWorkspace object or URL opening API is called, so it cannot launch an app,
// open Finder, activate a window, or change the user's document.
@interface IKViewTestWorkspace : NSObject
@property(nonatomic, strong) NSURL *application;
@property(nonatomic, strong) NSArray<NSURL *> *requests;
@property(nonatomic, strong) NSURL *receivedApplication;
@property(nonatomic) NSUInteger deliveries;
@end
@implementation IKViewTestWorkspace
- (NSURL *)URLForApplicationWithBundleIdentifier:(NSString *)identifier {
    return [identifier isEqualToString:@"io.ikview.desktop"] ? self.application : nil;
}
- (void)openURLs:(NSArray<NSURL *> *)urls withApplicationAtURL:(NSURL *)applicationURL
    configuration:(NSWorkspaceOpenConfiguration *)configuration
    completionHandler:(void (^)(NSRunningApplication *, NSError *))completion {
    (void)configuration;
    self.requests = urls;
    self.receivedApplication = applicationURL;
    ++self.deliveries;
    if (completion) completion(nil,nil);
}
@end

static int checks = 0;
static void check(bool value, NSString *description) {
    ++checks;
    if (!value) { fprintf(stderr, "FAIL: %s\n", description.UTF8String); exit(1); }
}

int main() {
    @autoreleasepool {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSURL *root = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]
                                isDirectory:YES];
        [fm createDirectoryAtURL:root withIntermediateDirectories:YES attributes:nil error:nil];
        NSURL *folder = [root URLByAppendingPathComponent:@"폴더 ' $(literal)" isDirectory:YES];
        [fm createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
        NSURL *photo = [folder URLByAppendingPathComponent:@"사진 ' $(literal)\n1.PNG"];
        NSURL *pdf = [folder URLByAppendingPathComponent:@"문서.pdf"];
        NSURL *unknown = [folder URLByAppendingPathComponent:@"README.md"];
        for (NSURL *url in @[photo, pdf, unknown]) [@"fixture" writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:nil];
        NSURL *remote = [NSURL URLWithString:@"https://example.invalid/photo.jpg"];
        const auto items = FIMenuKindContextualMenuForItems;
        const auto background = FIMenuKindContextualMenuForContainer;
        check([IKViewFinderOpenURLs(@[photo, pdf, folder, photo, remote, unknown], nil, items)
               isEqualToArray:@[photo, pdf, folder]], @"supported items preserve order, unicode and literal paths");
        check([IKViewFinderFolderURLs(@[photo, pdf, unknown, folder], nil, items)
               isEqualToArray:@[folder]], @"folder command deduplicates parents");
        check([IKViewFinderOpenURLs(@[], photo, items) isEqualToArray:@[photo]], @"target fallback");
        check(IKViewFinderOpenURLs(@[], nil, items).count == 0, @"missing selection");
        check(IKViewFinderFolderURLs(@[remote], nil, items).count == 0, @"remote URL rejected");
        check([IKViewFinderFolderURLs(@[photo], folder, background) isEqualToArray:@[folder]], @"background targets current folder");
        check([IKViewFinderFolderURLs(@[photo], folder, FIMenuKindContextualMenuForSidebar) isEqualToArray:@[folder]], @"sidebar uses clicked folder");
        check(IKViewFinderOpenURLs(@[photo], folder, FIMenuKindToolbarItemMenu).count == 0, @"no Finder toolbar injection");
        // Do not call init: only the real extension host may create its shared
        // FIFinderSyncController. These menu methods do not depend on ivars.
        IKViewFinder *extension = [IKViewFinder alloc];
        NSMenu *menu = [extension menuForKind:items selection:@[photo, pdf] target:photo];
        check(menu.numberOfItems == 2, @"selected files get two direct menu items");
        check([menu.itemArray[0].title isEqualToString:@"IKView로 열기"], @"direct open menu title");
        check([menu.itemArray[1].title isEqualToString:@"IKView로 폴더 열기"], @"direct folder menu title");
        check(menu.itemArray[0].submenu == nil && menu.itemArray[1].submenu == nil, @"items are not nested Services entries");
        check([[extension snapshotURLsForMenuItem:menu.itemArray[0]] isEqualToArray:@[photo, pdf]], @"menu captures immutable file selection");
        NSMenuItem *remoteCopy = [[NSMenuItem alloc] initWithTitle:menu.itemArray[0].title action:menu.itemArray[0].action keyEquivalent:@""];
        remoteCopy.tag = menu.itemArray[0].tag;
        check(remoteCopy.representedObject == nil && [[extension snapshotURLsForMenuItem:remoteCopy] isEqualToArray:@[photo,pdf]], @"remote menu item resolves selection using supported tag alone");
        NSMenu *unknownMenu = [extension menuForKind:items selection:@[unknown] target:unknown];
        check(unknownMenu.numberOfItems == 1 && [unknownMenu.itemArray[0].title containsString:@"폴더"], @"unsupported files offer containing folder only");
        NSMenu *backgroundMenu = [extension menuForKind:background selection:@[photo] target:folder];
        check(backgroundMenu.numberOfItems == 1 && [[extension snapshotURLsForMenuItem:backgroundMenu.itemArray[0]] isEqualToArray:@[folder]], @"background menu opens current directory");
        check([[extension snapshotURLsForMenuItem:remoteCopy] isEqualToArray:@[photo,pdf]], @"new menu cannot overwrite an older menu's file selection");
        for (int i=0;i<20;++i) [extension menuForKind:items selection:@[photo] target:photo];
        check([extension snapshotURLsForMenuItem:remoteCopy].count == 0, @"old snapshots are bounded and expire without retargeting");
        check([extension menuForKind:items selection:@[remote] target:remote] == nil, @"no menu for remote selection");
        NSURL *app = [root URLByAppendingPathComponent:@"IKView.app" isDirectory:YES];
        NSURL *contents = [app URLByAppendingPathComponent:@"Contents" isDirectory:YES];
        [fm createDirectoryAtURL:contents withIntermediateDirectories:YES attributes:nil error:nil];
        NSDictionary *info = @{@"CFBundleIdentifier": @"io.ikview.desktop", @"CFBundlePackageType": @"APPL"};
        [info writeToURL:[contents URLByAppendingPathComponent:@"Info.plist"] atomically:YES];
        NSURL *appex = [contents URLByAppendingPathComponent:@"PlugIns/IKViewFinder.appex" isDirectory:YES];
        check([IKViewFinderContainingApplication(appex) isEqual:app], @"containing app resolved without global registration lookup");
        [fm removeItemAtURL:[contents URLByAppendingPathComponent:@"Info.plist"] error:nil];
        check([IKViewFinderContainingApplication(appex) isEqual:app], @"sandbox does not need to read containing app Info.plist");
        check(IKViewFinderContainingApplication([contents URLByAppendingPathComponent:@"PlugIns/Other.appex"]) == nil, @"only own extension name is accepted");
        check(IKViewFinderContainingApplication([NSURL URLWithString:@"https://example.invalid/IKView.app/Contents/PlugIns/IKViewFinder.appex"]) == nil, @"remote bundle URL rejected");
        check(IKViewFinderContainingApplication([root URLByAppendingPathComponent:@"not-an-app/Contents/PlugIns/IKViewFinder.appex"]) == nil, @"containing directory must be an application bundle");
        check(IKViewFinderContainingApplication([root URLByAppendingPathComponent:@"other.appex"]) == nil, @"incorrect embedding rejected");
        NSURL *request = IKViewFinderHandoffURL(@[photo, pdf, folder]);
        NSURLComponents *requestParts = [NSURLComponents componentsWithURL:request resolvingAgainstBaseURL:NO];
        check([requestParts.scheme isEqualToString:@"ikview"] && [requestParts.host isEqualToString:@"open"], @"handoff is an app link instead of sandbox-restricted file URL");
        check([requestParts.queryItems[0].value isEqualToString:photo.path] && [requestParts.queryItems[1].value isEqualToString:pdf.path] && [requestParts.queryItems[2].value isEqualToString:folder.path], @"handoff preserves literal unicode paths and order");
        check(IKViewFinderHandoffURL(@[remote]) == nil, @"handoff refuses non-file URLs");
        check(IKViewFinderHandoffURL(@[]) == nil, @"handoff refuses empty selection");
        NSURL *literal = [NSURL fileURLWithPath:@"/tmp/a+b%2Fc&d?.png"];
        NSURL *wire = IKViewFinderHandoffURL(@[photo,literal]);
        check(ik::pathsFromFinderHandoff(QUrl::fromEncoded(QByteArray(wire.absoluteString.UTF8String))) ==
            QStringList({QString::fromUtf8(photo.path.UTF8String), QString::fromUtf8(literal.path.UTF8String)}),
            @"actual Foundation encoder and Qt decoder preserve literal paths end to end");
        IKViewTestWorkspace *workspace = [[IKViewTestWorkspace alloc] init];
        workspace.application = app;
        Method sharedMethod = class_getClassMethod(NSWorkspace.class, @selector(sharedWorkspace));
        IMP originalWorkspace = method_getImplementation(sharedMethod);
        IMP mockWorkspace = imp_implementationWithBlock(^id(id) { return workspace; });
        method_setImplementation(sharedMethod,mockWorkspace);
        NSMenu *commandMenu = [extension menuForKind:items selection:@[photo,literal] target:photo];
        NSMenuItem *remoteCommand = [[NSMenuItem alloc] initWithTitle:commandMenu.itemArray[0].title action:commandMenu.itemArray[0].action keyEquivalent:@""];
        remoteCommand.tag = commandMenu.itemArray[0].tag;
        [extension openSelection:remoteCommand];
        check(workspace.deliveries == 1 && workspace.requests.count == 1 &&
            [workspace.requests[0].scheme isEqualToString:@"ikview"], @"remote command delivers only app URI, never file URLs requiring sandbox access");
        check([workspace.receivedApplication isEqual:app], @"remote command uses the resolved viewer application");
        check(ik::pathsFromFinderHandoff(QUrl::fromEncoded(QByteArray(workspace.requests[0].absoluteString.UTF8String))) ==
            QStringList({QString::fromUtf8(photo.path.UTF8String), QString::fromUtf8(literal.path.UTF8String)}),
            @"real selection action reaches strict Qt decoder with original paths");
        remoteCommand.tag = commandMenu.itemArray[1].tag;
        [extension openSelection:remoteCommand];
        check(workspace.deliveries == 2 && ik::pathsFromFinderHandoff(QUrl::fromEncoded(QByteArray(workspace.requests[0].absoluteString.UTF8String))) ==
            QStringList({QString::fromUtf8(folder.path.UTF8String), QStringLiteral("/tmp")}),
            @"folder action delivers deduplicated containing folders without opening GUI");
        method_setImplementation(sharedMethod,originalWorkspace);
        imp_removeBlock(mockWorkspace);
        [fm removeItemAtURL:root error:nil];
        printf("Finder extension: %d checks passed\n", checks);
    }
}
