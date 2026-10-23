#import "IKViewFinder.h"
#import <AppKit/AppKit.h>
#import <os/log.h>
#include <limits>

namespace {
bool directory(NSURL *url) {
    if (!url.isFileURL) return false;
    NSNumber *value = nil;
    if ([url getResourceValue:&value forKey:NSURLIsDirectoryKey error:nil])
        return value.boolValue;
    // Finder's own directory URLs can still be useful without permission to
    // enumerate their contents. The receiver app owns all file access prompts.
    return url.hasDirectoryPath;
}

NSArray<NSURL *> *candidates(NSArray<NSURL *> *selection, NSURL *target, FIMenuKind kind) {
    if (kind == FIMenuKindContextualMenuForContainer || kind == FIMenuKindContextualMenuForSidebar)
        return target.isFileURL ? @[target] : @[];
    if (kind != FIMenuKindContextualMenuForItems) return @[];
    return selection.count ? selection : (target.isFileURL ? @[target] : @[]);
}

NSSet<NSString *> *supportedExtensions() {
    static NSSet<NSString *> *extensions;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        extensions = [NSSet setWithArray:@[@"jpg", @"jpeg", @"jpe", @"png", @"gif", @"bmp",
            @"dib", @"webp", @"tif", @"tiff", @"heic", @"heif", @"avif", @"svg", @"svgz",
            @"ico", @"icns", @"pbm", @"pgm", @"ppm", @"xbm", @"xpm", @"tga", @"jp2",
            @"j2k", @"jpf", @"jpx", @"jpm", @"jxl", @"exr", @"hdr", @"psd", @"pdf",
            @"zip", @"cbz", @"rar", @"cbr", @"7z", @"cb7", @"tar", @"cbt", @"gz",
            @"bz2", @"xz", @"tgz", @"tbz", @"tbz2", @"txz"]];
    });
    return extensions;
}

NSArray<NSURL *> *plan(NSArray<NSURL *> *selection, NSURL *target, FIMenuKind kind, bool folders) {
    NSMutableOrderedSet<NSURL *> *result = [NSMutableOrderedSet orderedSet];
    for (NSURL *url in candidates(selection, target, kind)) {
        if (![url isKindOfClass:NSURL.class] || !url.isFileURL) continue;
        const bool isDirectory = directory(url);
        NSURL *resolved = url.URLByStandardizingPath;
        if (folders) {
            if (!isDirectory) resolved = resolved.URLByDeletingLastPathComponent;
        } else if (!isDirectory && ![supportedExtensions() containsObject:url.pathExtension.lowercaseString]) {
            continue;
        }
        if (resolved) [result addObject:resolved];
    }
    return result.array;
}
}

NSArray<NSURL *> *IKViewFinderOpenURLs(NSArray<NSURL *> *selection, NSURL *target, FIMenuKind kind) {
    return plan(selection, target, kind, false);
}
NSArray<NSURL *> *IKViewFinderFolderURLs(NSArray<NSURL *> *selection, NSURL *target, FIMenuKind kind) {
    return plan(selection, target, kind, true);
}

NSURL *IKViewFinderContainingApplication(NSURL *extensionURL) {
    if (!extensionURL.isFileURL || ![extensionURL.lastPathComponent isEqualToString:@"IKViewFinder.appex"]) return nil;
    NSURL *contents = extensionURL.URLByDeletingLastPathComponent.URLByDeletingLastPathComponent;
    if (![extensionURL.pathExtension isEqualToString:@"appex"] ||
        ![extensionURL.URLByDeletingLastPathComponent.lastPathComponent isEqualToString:@"PlugIns"] ||
        ![contents.lastPathComponent isEqualToString:@"Contents"]) return nil;
    NSURL *application = contents.URLByDeletingLastPathComponent;
    // The extension has access to its own bundle, but its sandbox need not
    // permit reading the containing app's Info.plist. Derive the parent of our
    // trusted code bundle structurally; Launch Services performs the launch.
    return [application.pathExtension isEqualToString:@"app"] ? application : nil;
}

NSURL *IKViewFinderHandoffURL(NSArray<NSURL *> *urls) {
    if (!urls.count || urls.count > 10000) return nil;
    NSURLComponents *components = [[NSURLComponents alloc] init];
    components.scheme = @"ikview";
    components.host = @"open";
    NSMutableArray<NSURLQueryItem *> *items = [NSMutableArray arrayWithCapacity:urls.count];
    for (NSURL *url in urls) {
        if (!url.isFileURL || !url.path.isAbsolutePath || [url.path hasPrefix:@"//"]) return nil;
        [items addObject:[NSURLQueryItem queryItemWithName:@"path" value:url.path]];
    }
    components.queryItems = items;
    NSURL *request = components.URL;
    return [request.absoluteString lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= 1024 * 1024 ? request : nil;
}

@interface IKViewFinder ()
@property(nonatomic, strong) NSMutableDictionary<NSNumber *, NSArray<NSURL *> *> *menuSnapshots;
@property(nonatomic, strong) NSMutableArray<NSNumber *> *snapshotOrder;
@property(nonatomic) NSInteger nextMenuTag;
@end

@implementation IKViewFinder
- (instancetype)init {
    self = [super init];
    if (self) {
        // This is only a Finder UI scope, never a filesystem scan or watcher.
        // A root scope includes ordinary folders and newly mounted volumes.
        [FIFinderSyncController defaultController].directoryURLs =
            [NSSet setWithObject:[NSURL fileURLWithPath:@"/" isDirectory:YES]];
    }
    return self;
}

- (NSMenu *)menuForMenuKind:(FIMenuKind)kind {
    FIFinderSyncController *controller = [FIFinderSyncController defaultController];
    NSArray<NSURL *> *selection = controller.selectedItemURLs;
    return [self menuForKind:kind selection:selection ? selection : @[] target:controller.targetedURL];
}

- (NSMenu *)menuForKind:(FIMenuKind)kind selection:(NSArray<NSURL *> *)selection target:(NSURL *)target {
    if (kind == FIMenuKindToolbarItemMenu) return nil;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"IKView"];
    menu.autoenablesItems = NO;
    NSArray<NSURL *> *files = IKViewFinderOpenURLs(selection, target, kind);
    NSArray<NSURL *> *folders = IKViewFinderFolderURLs(selection, target, kind);
    if (files.count && kind == FIMenuKindContextualMenuForItems) {
        NSMenuItem *open = [menu addItemWithTitle:@"IKView로 열기" action:@selector(openSelection:) keyEquivalent:@""];
        open.target = self;
        open.tag = [self retainSnapshot:files];
        open.image = [NSImage imageWithSystemSymbolName:@"photo" accessibilityDescription:nil];
    }
    if (folders.count) {
        NSMenuItem *open = [menu addItemWithTitle:@"IKView로 폴더 열기" action:@selector(openSelection:) keyEquivalent:@""];
        open.target = self;
        open.tag = [self retainSnapshot:folders];
        open.image = [NSImage imageWithSystemSymbolName:@"folder" accessibilityDescription:nil];
    }
    return menu.numberOfItems ? menu : nil;
}

- (NSInteger)retainSnapshot:(NSArray<NSURL *> *)urls {
    if (!self.menuSnapshots) {
        self.menuSnapshots = [NSMutableDictionary dictionary];
        self.snapshotOrder = [NSMutableArray array];
    }
    if (self.nextMenuTag == std::numeric_limits<NSInteger>::max()) {
        [self.menuSnapshots removeAllObjects];
        [self.snapshotOrder removeAllObjects];
        self.nextMenuTag = 0;
    }
    NSNumber *tag = @(++self.nextMenuTag);
    self.menuSnapshots[tag] = [urls copy];
    [self.snapshotOrder addObject:tag];
    // Finder serializes a fresh NSMenuItem and does not transport an arbitrary
    // representedObject. Its supported integer tag addresses a bounded local
    // snapshot; opening another menu cannot retarget a previous menu's action.
    while (self.snapshotOrder.count > 32) {
        NSNumber *oldest = self.snapshotOrder.firstObject;
        [self.menuSnapshots removeObjectForKey:oldest];
        [self.snapshotOrder removeObjectAtIndex:0];
    }
    return tag.integerValue;
}

- (NSArray<NSURL *> *)snapshotURLsForMenuItem:(NSMenuItem *)item {
    NSArray<NSURL *> *urls = self.menuSnapshots[@(item.tag)];
    return urls ? urls : @[];
}

- (void)openSelection:(NSMenuItem *)sender {
    // Snapshot URLs belong to the menu that was clicked. A different Finder
    // window becoming active cannot retarget an already displayed command.
    NSArray<NSURL *> *urls = [self snapshotURLsForMenuItem:sender];
    os_log_info(OS_LOG_DEFAULT, "IKView Finder: command tag %ld, %lu URLs", (long)sender.tag, (unsigned long)urls.count);
    if (!urls.count) {
        os_log_error(OS_LOG_DEFAULT, "IKView Finder: command selection expired or missing");
        return;
    }
    for (id url in urls) if (![url isKindOfClass:NSURL.class] || ![url isFileURL]) return;
    NSURL *codeBundle = [NSBundle bundleForClass:IKViewFinder.class].bundleURL;
    NSURL *mainBundle = NSBundle.mainBundle.bundleURL;
    NSURL *app = IKViewFinderContainingApplication(codeBundle);
    if (!app) app = IKViewFinderContainingApplication(mainBundle);
    if (!app) app = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"io.ikview.desktop"];
    os_log_info(OS_LOG_DEFAULT, "IKView Finder: code bundle %{public}@, main bundle %{public}@, app %{public}@",
                codeBundle.path, mainBundle.path, app.path);
    if (!app) {
        os_log_error(OS_LOG_DEFAULT, "IKView Finder: containing application could not be resolved");
        return;
    }
    NSWorkspaceOpenConfiguration *configuration = [NSWorkspaceOpenConfiguration configuration];
    configuration.activates = YES;
    NSURL *request = IKViewFinderHandoffURL(urls);
    if (!request) {
        os_log_error(OS_LOG_DEFAULT, "IKView Finder: open request is invalid or too large");
        return;
    }
    // Finder metadata URLs do not grant this sandbox permission to open the
    // selected documents. A read-only deep link hands the paths to the main
    // viewer, which already owns its ordinary macOS file access permissions.
    [[NSWorkspace sharedWorkspace] openURLs:@[request] withApplicationAtURL:app configuration:configuration
        completionHandler:^(NSRunningApplication *, NSError *error) {
            if (error) os_log_error(OS_LOG_DEFAULT, "IKView Finder: open failed: %{public}@ code %ld info %{public}@", error.domain, (long)error.code, error.userInfo);
            else os_log_info(OS_LOG_DEFAULT, "IKView Finder: open request delivered");
        }];
}
@end
