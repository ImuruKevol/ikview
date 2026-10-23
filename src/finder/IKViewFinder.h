#import <FinderSync/FinderSync.h>

NS_ASSUME_NONNULL_BEGIN

// Pure selection planning also used by the native, disposable-fixture tests.
NSArray<NSURL *> *IKViewFinderOpenURLs(NSArray<NSURL *> *selection, NSURL * _Nullable target,
                                      FIMenuKind kind);
NSArray<NSURL *> *IKViewFinderFolderURLs(NSArray<NSURL *> *selection, NSURL * _Nullable target,
                                        FIMenuKind kind);
NSURL * _Nullable IKViewFinderContainingApplication(NSURL *extensionURL);
NSURL * _Nullable IKViewFinderHandoffURL(NSArray<NSURL *> *urls);

@interface IKViewFinder : FIFinderSync
- (nullable NSMenu *)menuForKind:(FIMenuKind)kind selection:(NSArray<NSURL *> *)selection
                         target:(nullable NSURL *)target;
- (NSArray<NSURL *> *)snapshotURLsForMenuItem:(NSMenuItem *)item;
@end

NS_ASSUME_NONNULL_END
