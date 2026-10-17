// Install an app into the booted device through installd, as simctl install does, then open it through SpringBoard,
// as simctl launch does. LaunchServices refuses to install on iOS ("Use InstallCoordination").
// installd moves what it installs into its staging directory: install a copy.
// usage: siminstall PATH.app|- BUNDLE_ID (- only opens the app)
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>

@interface MIAppIdentity : NSObject
- (instancetype)initWithBundleID:(NSString*)bundleID;
@end
@interface MIInstallOptions : NSObject
- (instancetype)initWithLegacyOptionsDictionary:(NSDictionary*)options;
- (void)setBundleIdentifier:(NSString*)bundleID;
@end
@interface MIInstallerClient : NSObject
+ (instancetype)installerWithProgressBlock:(void (^)(NSDictionary*))block;
- (void)installURL:(NSURL*)url
			identity:(MIAppIdentity*)identity
	 targetingDomain:(unsigned long long)domain
		 withOptions:(MIInstallOptions*)options
 returningResultInfo:(BOOL)resultInfo
		  completion:(void (^)(BOOL, NSArray*, id, NSError*))completion;
@end
extern int SBSLaunchApplicationWithIdentifier(CFStringRef identifier, Boolean suspended);

// the user's apps' domain (MIInstallationDomain)
#define DOMAIN_USER 2

static BOOL install(NSString* path, NSString* bundleID)
{
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	__block BOOL installed = NO;
	MIInstallerClient* client = [NSClassFromString(@"MIInstallerClient") installerWithProgressBlock:^(NSDictionary* p) {
		fprintf(stderr, "siminstall: %s %s\n", [p[@"Status"] description].UTF8String,
			[p[@"PercentComplete"] description].UTF8String);
	}];
	MIInstallOptions* options =
		[[NSClassFromString(@"MIInstallOptions") alloc] initWithLegacyOptionsDictionary:@{ @"PackageType" : @"Developer" }];
	options.bundleIdentifier = bundleID;
	[client installURL:[NSURL fileURLWithPath:path]
				identity:[[NSClassFromString(@"MIAppIdentity") alloc] initWithBundleID:bundleID]
		 targetingDomain:DOMAIN_USER
			 withOptions:options
	 returningResultInfo:YES
			  completion:^(BOOL ok, NSArray* records, id promise, NSError* err) {
				  fprintf(stderr, "siminstall: install %s: %d %s\n", path.UTF8String, ok,
					  ok ? "" : err.description.UTF8String);
				  installed = ok;
				  dispatch_semaphore_signal(done);
			  }];
	dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
	return installed;
}

int main(int argc, char** argv)
{
	@autoreleasepool
	{
		if (argc != 3)
		{
			fprintf(stderr, "usage: siminstall PATH.app|- BUNDLE_ID\n");
			return 2;
		}
		NSString* bundleID = @(argv[2]);
		if (strcmp(argv[1], "-") != 0 && !install(@(argv[1]), bundleID))
			return 1;
		int rv = SBSLaunchApplicationWithIdentifier((__bridge CFStringRef) bundleID, false);
		fprintf(stderr, "siminstall: open %s: %d\n", argv[2], rv);
		return rv ? 1 : 0;
	}
}
