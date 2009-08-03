//
//  AFSCommanderPref.h
//  AFSCommander
//
//  Created by Claudio Bisegni on 10/05/07.
//  Copyright (c) 2007 INFN - National Institute of Nuclear Physics. All rights reserved.
//

#import <PreferencePanes/PreferencePanes.h>
#import "AFSPropertyManager.h"
#import "global.h"
#import "ViewUtility.h"
#import "LynkCreationController.h"

// the way to load and unload the menuextra was inspired by MenuMeters developed by Alex Harper
// Routines to handle adding and remove menu extras in HIServices (from ASM source)
int CoreMenuExtraGetMenuExtra(CFStringRef identifier, void *menuExtra);
int CoreMenuExtraAddMenuExtra(CFURLRef path, int position, int whoCares, int whoCares2, int whoCares3, int whoCares4);
int CoreMenuExtraRemoveMenuExtra(void *menuExtra, int whoCares);



@interface AFSCommanderPref : NSPreferencePane 
{
	//for check system version
	int prefStartUp;
	// Main View
	BOOL startAFSAtLogin;
	IBOutlet NSView *afsCommanderView;
	IBOutlet NSSearchField *textSearchField;
	IBOutlet NSTextField *afsDefaultCellLabel;
	IBOutlet NSButton *tokensButton;
	IBOutlet NSButton *unlogButton;
	IBOutlet NSButton *aklogCredentialAtLoginTime;
	IBOutlet NSButton *installKRB5AuthAtLoginButton;
	IBOutlet NSButton *useAklogCheck;
	IBOutlet NSTextField *afsVersionLabel;
	IBOutlet NSButton *checkButtonAfsAtBootTime;
	IBOutlet NSTextField *textFieldDevInfoLabel;
	IBOutlet NSTextField *statCacheEntry;
	IBOutlet NSTextField *dCacheDim;
	IBOutlet NSTextField *cacheDimension;
	IBOutlet NSTextField *daemonNumber;
	IBOutlet NSTextField *afsRootMountPoint;
	IBOutlet NSTextField *nVolEntry;
	IBOutlet NSButton *dynRoot;
	IBOutlet NSButton *afsDB;
	IBOutlet NSButton *verbose;
	IBOutlet NSButton *backgrounderActivationCheck;
	IBOutlet NSBox *groupsBox;
	
	//id installationPathTextField;
	id startStopButton;
	id cellList;
	//id cellNameTextEdit;
	id cellIpButton;
	id addCellButton;
	id removeCellButton;
	//id refreshConfigurationButton;
	id saveConfigurationButton;
	id labelSaveResult;
	id tokensTable;
	id afsMenucheckBox;
		
	//Configuration sheet
	id ipConfigurationSheet;
	id ipConfControllerCommander;
	
	//Token sheet
	id credentialSheet;
	id credentialCommander;	

	
	//Info Sheet
	id infoSheet;
	id infoController;	

	//lynk creation
	id lyncCreationSheet;
	IBOutlet LynkCreationController		*lynkCreationController;
	
	//manage link
	IBOutlet NSButton					*checkEnableLink;
	IBOutlet NSButton					*buttonAddLink;
	IBOutlet NSButton					*buttonRemoveLink;
	bool enableLink;
	
	AFSPropertyManager *afsProperty;	//AFS Property managment class
	NSMutableArray *filteredCellDB;		//Filtered CellServDB
	NSArray *tokenList;
	NSTimer *timerForCheckTokensList;
	NSLock *tokensLock;
}

- (void) mainViewDidLoad;
- (void) willUnselect;
- (void) didSelect;
- (id) initWithBundle:(NSBundle *)bundle;
- (void)startTimer;
- (void)stopTimer;
//View Action
- (IBAction) refreshConfiguration:(id) sender;
- (void) fillCacheParamView;
- (void) updateCacheParamFromView;
- (IBAction) showCellIP:(id) sender;
- (IBAction) addRemoveCell:(id) sender;
- (IBAction) addLink:(id) sender;
- (IBAction) removeLink:(id) sender;
- (IBAction) enableLink:(id) sender;
- (IBAction) saveConfiguration:(id) sender;
- (IBAction) saveCacheManagerParam:(id) sender;
- (IBAction) startStopAfs:(id) sender;
- (IBAction) info:(id) sender;
- (IBAction) tableDoubleAction:(id) sender;
- (IBAction) getNewToken:(id) sender;
- (IBAction) unlog:(id) sender;
- (IBAction) afsMenuActivationEvent:(id) sender;
- (IBAction) aklogSwitchEvent:(id) sender;
- (IBAction) credentialAtLoginTimeEvent:(id) sender;
- (IBAction) afsStartupSwitchEvent:(id) sender;
- (IBAction) krb5KredentialAtLoginTimeEvent:(id) sender;
- (IBAction) searchCellTextEvent:(id) sender;
- (IBAction) manageBackgrounderActivation:(id)sender;
- (void) credentialAtLoginTimeEventCreationLaunchAgentDir:(NSWindow*)alert returnCode:(int)returnCode contextInfo:(void *)contextInfo;
- (void) clearCellServDBFiltering;
- (void) filterCellServDB:(NSString*)textToFilter;
- (DBCellElement*) getCurrentCellInDB;
- (DBCellElement*) getCellByIDX:(int) idx;
- (void) modifyCell:(DBCellElement*) cellElement;
- (void) modifyCellByIDX:(int) idx;
- (void) showMessage:(NSString*) message;
- (void) manageButtonState:(int) rowSelected;
- (void) setAfsStatus;
- (void) refreshTokens:(NSTimer*)theTimer;
- (void) repairHelperTool;
- (void) writePreferenceFile;
- (void) readPreferenceFile;
- (void) refreshGui:(NSNotification *)notification;
- (void) afsVolumeMountChange:(NSNotification *)notification;
- (void)tabView:(NSTabView *)tabView willSelectTabViewItem: (NSTabViewItem *)tabViewItem;
@end

@interface AFSCommanderPref (NSTableDataSource)
- (id) getTableTokensListValue:(int) colId row:(int)row;
- (id) getTableCelListValue:(int) colId row:(int)row;
@end;