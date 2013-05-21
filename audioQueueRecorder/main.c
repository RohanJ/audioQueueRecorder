//
//  main.c
//  audioQueueRecorder
//
//  Created by Rohan Jyoti on 5/13/12.
//  Copyright (c) 2012 __MyCompanyName__. All rights reserved.
//

#include <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioToolbox.h>

#define defOp "default errCheck operation"
#define kNumberRecordBuffers 5 //must be at least 3

#ifndef audioQueueRecorder_h
#define audioQueueRecorder_h
static void errCheck(OSStatus error, const char *operation);
OSStatus getInputDeviceSampleRate(Float64 *outSampleRate);
static void AQInputCallback(void *inUserData, AudioQueueRef inQueue, 
							AudioQueueBufferRef inBuffer, 
							const AudioTimeStamp *inStartTime, 
							UInt32 inNumPackets, 
							const AudioStreamPacketDescription *inPacketDescription);
static void mCopyEncoderCookieToFile(AudioQueueRef aQ, AudioFileID theFile);
static int mComputeRecordBufferSize(const AudioStreamBasicDescription *format, 
									AudioQueueRef queue, 
									float seconds);
#endif

#pragma mark user data struct
typedef struct mRec
{
	AudioFileID			recFile;
	SInt64				recPacket;
	Boolean				running;
}mRec;

#pragma mark utility functions
static void errCheck(OSStatus error, const char *operation)
{
	if(error == noErr) return;
	
	char errorString[20];
	*(UInt32 *)(errorString + 1) = CFSwapInt32HostToBig(error);
	//see if 4-char code
	if(isprint(errorString[1]) && isprint(errorString[2]) && isprint(errorString[3]) && isprint(errorString[4]))
	{
		errorString[0] = errorString[5] = '\'';
		errorString[6] = '\0';
	}
	else
		sprintf(errorString, "%d", (int)error); //format as integer instead
	
	fprintf(stderr, "Error: %s (%s)\n", operation, errorString);
	exit(1);
}

OSStatus getInputDeviceSampleRate(Float64 *outSampleRate)
{
	OSStatus error;
	AudioDeviceID deviceID = 0;
	
	//first we must figure out the deviceID
	AudioObjectPropertyAddress propertyAddress;
	UInt32 propertySize;
	propertyAddress.mSelector = kAudioHardwarePropertyDefaultInputDevice; //original
	//propertyAddress.mSelector = kAudioHardwarePropertyDefaultOutputDevice; //test 1
	//propertyAddress.mSelector = kAudioHardwarePropertyDefaultSystemOutputDevice; //test 2
	//Neither test 1 or test 2 worked to get ouput as input. Next step is to look at soundflower source code.
	propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
	propertyAddress.mElement = 0;
	propertySize = sizeof(AudioDeviceID);
	error = AudioHardwareServiceGetPropertyData(kAudioObjectSystemObject, 
												&propertyAddress, 0, NULL, &propertySize, &deviceID);
	if(error) return error;
	printf("deviceId: %d\n", deviceID);

	//now that we gotten the deviceID, we can finally investigate the device to get its default sample rate
	propertyAddress.mSelector = kAudioDevicePropertyNominalSampleRate;
	propertyAddress.mScope = kAudioObjectPropertyScopeGlobal;
	propertyAddress.mElement = 0;
	propertySize = sizeof(Float64);
	error = AudioHardwareServiceGetPropertyData(deviceID, &propertyAddress, 0, NULL, &propertySize, outSampleRate);
	return error;
}

static void mCopyEncoderCookieToFile(AudioQueueRef aQ, AudioFileID theFile)
{
	OSStatus error;
	UInt32 propertySize;
	
	error = AudioQueueGetPropertySize(aQ,
								  kAudioConverterCompressionMagicCookie,
									  &propertySize);
	if(error == noErr && propertySize>0)
	{
		Byte *magicCookie = (Byte *)malloc(propertySize);
		errCheck(AudioQueueGetProperty(aQ, 
									   kAudioQueueProperty_MagicCookie,
									   magicCookie, &propertySize), "Audio Queue Magic Cookie Retrieval Error");
		errCheck(AudioFileSetProperty(theFile, 
									  kAudioFilePropertyMagicCookieData, 
									  propertySize, 
									  magicCookie), "Audio File Magic Cookie Retrieval Error");
		free(magicCookie);
	}
}

static int mComputeRecordBufferSize(const AudioStreamBasicDescription *format, 
									AudioQueueRef queue, 
									float seconds)
{
	int packets, frames, bytes;
	
	//recall that format->mSampleRate is getting the mSampleRate from the format struct, kind of like c++ deference
	frames = (int)ceil(seconds * format->mSampleRate);
	
	if(format->mBytesPerFrame > 0) 
		bytes = frames * format->mBytesPerFrame; //meaning mBytesPerFram is defined
	else
	{
		//meaning mBytesPerFrame was not filled out in the ASBD
		UInt32 maxPacketSize;
		if(format->mBytesPerPacket > 0) 
			maxPacketSize = format->mBytesPerPacket; //meaning there is a constant packet size
		else
		{
			//We must obtain the largest single size packet possible
			UInt32 propertySize = sizeof(maxPacketSize); //currently just sizeof UInt32
			errCheck(AudioQueueGetProperty(queue, 
										   kAudioConverterPropertyMaximumOutputPacketSize, 
										   &maxPacketSize, 
										   &propertySize), "Queue maximum output packet size error");
		}
		
		if(format->mFramesPerPacket > 0)
			packets = frames / format->mFramesPerPacket;
		else
			packets = frames; //worst-case scenerio: 1 frame in a packet
		
		//base cases
		if(packets == 0)
			packets = 1;
	
		bytes = packets * maxPacketSize;
	}
	
	return bytes;	
}

#pragma mark record callback functions
static void AQInputCallback(void *inUserData, AudioQueueRef inQueue, AudioQueueBufferRef inBuffer, 
							const AudioTimeStamp *inStartTime, UInt32 inNumPackets, 
							const AudioStreamPacketDescription *inPacketDescription)
{
	mRec *mRec1 = (mRec *)inUserData;
	
	if(inNumPackets > 0)
	{
		//write packets to a file
		errCheck(AudioFileWritePackets(mRec1->recFile, FALSE, inBuffer->mAudioDataByteSize, inPacketDescription, mRec1->recPacket, &inNumPackets, inBuffer->mAudioData), "AudioFileWritePackets error");
		//increment the packet index
		mRec1->recPacket+=inNumPackets;
	}
	
	//re-enqueue a used buffer
	if(mRec1->running)
		errCheck(AudioQueueEnqueueBuffer(inQueue, inBuffer, 0, NULL), "AudioQueueEnqueueBuffer failed");
}

#pragma mark main function
int main (int argc, const char * argv[])
{
	//========== Set up format ==========
	mRec mRec1 = {0};
	AudioStreamBasicDescription asbdRecFormat;
	//zero asbd out
	memset(&asbdRecFormat, 0, sizeof(asbdRecFormat));
	//recrod as stereo AAC
	asbdRecFormat.mFormatID = kAudioFormatMPEG4AAC;
	asbdRecFormat.mChannelsPerFrame = 2;
	//we could hard-code sample-rate, but if system sample rate is different, we may be forcing system to under/over work
	errCheck(getInputDeviceSampleRate(&asbdRecFormat.mSampleRate), "Device Sample Rate error");
	//Core Audio can complete the partially completed asbd if we call AudioFormatGetProperty with the kAudioFormatProperty_FormatInfo property
	//but we need at least mFormatID set in our partially filled asbd.
	UInt32 propSize = sizeof(asbdRecFormat);
	errCheck(AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 
									0, 
									NULL, 
									&propSize, 
									&asbdRecFormat), "AudioFormatGetProperty error");
	
	//========== Set up audio queue ==========
	AudioQueueRef aQ = {0};
	errCheck(AudioQueueNewInput(&asbdRecFormat, 
								AQInputCallback, 
								&mRec1, 
								NULL, 
								NULL, 
								0, 
								&aQ), "AudioQueueNewInput error"); 
	//the callback (function pointer to AQInputCallback) will be called every time
	//the audio queue fills the buffer
	UInt32 size = sizeof(asbdRecFormat);
	errCheck(AudioQueueGetProperty(aQ, 
								   kAudioConverterCurrentOutputStreamDescription, 
								   &asbdRecFormat, 
								   &size), "Couldn't get the queue's format");
	
	//========== Set up file ==========
	//First we create the audio file (mFileURL) for output
	CFURLRef mFileURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
													  CFSTR("output.caf"), 
													  kCFURLPOSIXPathStyle, 
													  false);
	errCheck(AudioFileCreateWithURL(mFileURL,
									kAudioFileCAFType,
									&asbdRecFormat,
									kAudioFileFlags_EraseFile,
									&mRec1.recFile), "AudioFileCreateWithURL error");
	CFRelease(mFileURL);
	
	//The audio queue gives us a magic cookie (a magic cookie is an opaque block of data that contains the values that are unique to a given codec
	//that the ASBD hasn't already accounted for. Some compressed formats use magic cookies, others do not; since we are using AAC in this program
	//and AAC makes use of magic cookies, we will make the following helper to retrieve the magic cookie
	mCopyEncoderCookieToFile(aQ, mRec1.recFile); //rec file is of type AudioFileID
	//========== Set up buffer ==========
	int bufferByteSize = mComputeRecordBufferSize(&asbdRecFormat, aQ, 0.5);
	int bufferIndex;
	for(bufferIndex=0; bufferIndex < kNumberRecordBuffers; bufferIndex++)
	{
		AudioQueueBufferRef buffer;
		errCheck(AudioQueueAllocateBuffer(aQ, 
										  bufferByteSize, 
										  &buffer), "AudioQueueAllocateBuffer error");
		errCheck(AudioQueueEnqueueBuffer(aQ, 
										 buffer, 
										 0, 
										 NULL), "AudioQueueEnqueueBuffer error");
		
	}
	
	//========== Start Queue ==========
	mRec1.running = TRUE;
	errCheck(AudioQueueStart(aQ, NULL), "AudioQueueStart error");
	
	//========== Stop Queue ==========
	printf("Currently recording. Press <return> to stop.\n");
	getchar(); //Note that on default, it will close on <return>
	
	printf("...Recording done.\n");
	mRec1.running=FALSE;
	errCheck(AudioQueueStop(aQ, TRUE), "AudioQueueStop error");
	
	//========== Final Cleanup ==========
	//In some cases, the magic cookie could be updated. In case we need the latest magic cookies, we recall our helper function.
	mCopyEncoderCookieToFile(aQ, mRec1.recFile);
	//Next we cleanup the audio queue and audio file
	AudioQueueDispose(aQ, TRUE);
	AudioFileClose(mRec1.recFile);
	
    return 0;
}

