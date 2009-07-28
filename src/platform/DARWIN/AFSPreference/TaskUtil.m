//
//  TaskUtil.m
//  AFSCommander
//
//  Created by Claudio Bisegni on 25/06/07.
//  Copyright 2007 INFN - National Institute of Nuclear Physics. All rights reserved.
//

#import "TaskUtil.h"


@implementation TaskUtil

// -------------------------------------------------------------------------------
//  executeTaskSearchingPath:
// -------------------------------------------------------------------------------
+(NSString*) executeTaskSearchingPath:(NSString*)unixCommand args:(NSArray*)args
{
	NSString *commResult = nil;
	NSString *commPath = [self searchExecutablePath:unixCommand];
	if(commPath != nil){
		commResult = [self executeTask:commPath
						   arguments:args];
	}
	return commResult;	
}

// -------------------------------------------------------------------------------
//  executeTask:
// -------------------------------------------------------------------------------
+(NSString*) searchExecutablePath:(NSString*)unixCommand
{
	NSString *commPath = [self executeTask:@"/usr/bin/which" arguments:[NSArray arrayWithObjects:unixCommand, nil]];
	return commPath;	
}

// -------------------------------------------------------------------------------
//  executeTask:
// -------------------------------------------------------------------------------
+(NSString*) executeTask:(NSString*) taskName arguments:(NSArray *)args{
	NSLog(taskName);
	NSString *result = nil;
	int status = 0;
	NSFileHandle *file = nil;
	NSDictionary *environment =  [NSDictionary dictionaryWithObjectsAndKeys: @"$PATH:/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin",@"PATH",nil];
		
	NSPipe *pipe = [NSPipe pipe];
	NSTask *taskToRun  = [[NSTask alloc] init];
	
	[taskToRun setLaunchPath: taskName];
	[taskToRun setArguments: args];
	[taskToRun setEnvironment:environment];
	[taskToRun setStandardOutput: pipe];
	file = [pipe fileHandleForReading];
	[taskToRun launch];
	[taskToRun waitUntilExit];
	status = [taskToRun terminationStatus];
	if (status == 0){
		NSLog(@"Task succeeded.");
		NSData *data = [file readDataToEndOfFile];
		// remove the \n character from unix command
		if([data length] > 0){
			NSData *realData = [NSData dataWithBytes:[data bytes] 
											  length:[data length]-1];
		
			[taskToRun release];
			result = [[NSString alloc] initWithData: realData 
										   encoding: NSUTF8StringEncoding];
			NSLog(result);
		}
	} else {
		NSLog(@"Task failed.");
	}
	return [result autorelease];
}


// -------------------------------------------------------------------------------
//  executeTask:
// -------------------------------------------------------------------------------
+(int) executeTaskWithAuth:(NSString*) taskName arguments:(NSArray *)args authExtForm:(NSData*)auth {
	NSLog(taskName);
	NSString *result = nil;
	int status = 0;
	NSFileHandle *file = nil;
	NSDictionary *environment =  [NSDictionary dictionaryWithObjectsAndKeys: @"$PATH:/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin",@"PATH",nil];
	
	NSPipe *pipe = [NSPipe pipe];
	NSPipe *pipeIn = [NSPipe pipe];
	NSTask *taskToRun  = [[NSTask alloc] init];
	
	[taskToRun setLaunchPath: taskName];
	[taskToRun setArguments: args];
	[taskToRun setEnvironment:environment];
	[taskToRun setStandardOutput: pipe];
	[taskToRun setStandardInput: pipeIn];
	file = [pipe fileHandleForReading];
	//Write to standard in
	[taskToRun launch];
	
	NSFileHandle* taskInput = [[ taskToRun standardInput ] fileHandleForWriting ];
	[taskInput writeData:auth];
	[taskToRun waitUntilExit];
	status = [taskToRun terminationStatus];
	if (status == 0){
		NSLog(@"Task succeeded.");
		NSData *data = [file readDataToEndOfFile];
		// remove the \n character from unix command
		if([data length] > 0){
			NSData *realData = [NSData dataWithBytes:[data bytes] 
											  length:[data length]-1];
			
			[taskToRun release];
			result = [[NSString alloc] initWithData: realData 
										   encoding: NSUTF8StringEncoding];
			NSLog(result);
			[result release];
		}
	} else {
		NSLog(@"Task failed.");
	}
	
	return status;
}

@end