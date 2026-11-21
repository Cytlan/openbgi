/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * AudioToolbox.h - a stand-in for the part of the macOS AudioToolbox
 * framework the POSIX back end uses (the AudioQueue output of
 * src/host/posix/audio.c), so that the host compiler can syntax-check the
 * macOS build without the SDK (`make check-macos`, docs/dev/building.md).  The
 * types, members and prototypes follow the SDK's headers; the constants'
 * values are those of the SDK.  Never used by the real build.
 */
#ifndef MACSTUB_AUDIOTOOLBOX_H_
#define MACSTUB_AUDIOTOOLBOX_H_

#include <stdint.h>

typedef uint32_t UInt32;
typedef double Float64;
typedef unsigned char Boolean;
typedef int32_t OSStatus;
typedef UInt32 AudioFormatID;
typedef UInt32 AudioFormatFlags;
typedef struct __CFRunLoop* CFRunLoopRef;
typedef const struct __CFString* CFStringRef;

enum
{
	noErr = 0
};
enum
{
	kAudioFormatLinearPCM = 0x6c70636d // 'lpcm'
};
enum
{
	kLinearPCMFormatFlagIsFloat = (1U << 0),
	kLinearPCMFormatFlagIsBigEndian = (1U << 1),
	kLinearPCMFormatFlagIsSignedInteger = (1U << 2),
	kLinearPCMFormatFlagIsPacked = (1U << 3)
};

typedef struct AudioStreamBasicDescription
{
	Float64 mSampleRate;
	AudioFormatID mFormatID;
	AudioFormatFlags mFormatFlags;
	UInt32 mBytesPerPacket;
	UInt32 mFramesPerPacket;
	UInt32 mBytesPerFrame;
	UInt32 mChannelsPerFrame;
	UInt32 mBitsPerChannel;
	UInt32 mReserved;
} AudioStreamBasicDescription;

typedef struct AudioStreamPacketDescription AudioStreamPacketDescription;
typedef struct AudioTimeStamp AudioTimeStamp;

typedef struct AudioQueueBuffer
{
	const UInt32 mAudioDataBytesCapacity;
	void* const mAudioData;
	UInt32 mAudioDataByteSize;
	void* mUserData;
	const UInt32 mPacketDescriptionCapacity;
	AudioStreamPacketDescription* const mPacketDescriptions;
	UInt32 mPacketDescriptionCount;
} AudioQueueBuffer;
typedef AudioQueueBuffer* AudioQueueBufferRef;
typedef struct OpaqueAudioQueue* AudioQueueRef;

typedef void (*AudioQueueOutputCallback)(void* inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer);

OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription* inFormat, AudioQueueOutputCallback inCallbackProc,
	void* inUserData, CFRunLoopRef inCallbackRunLoop, CFStringRef inCallbackRunLoopMode, UInt32 inFlags,
	AudioQueueRef* outAQ);
OSStatus AudioQueueAllocateBuffer(AudioQueueRef inAQ, UInt32 inBufferByteSize, AudioQueueBufferRef* outBuffer);
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer, UInt32 inNumPacketDescs,
	const AudioStreamPacketDescription* inPacketDescs);
OSStatus AudioQueueStart(AudioQueueRef inAQ, const AudioTimeStamp* inStartTime);
OSStatus AudioQueueStop(AudioQueueRef inAQ, Boolean inImmediate);
OSStatus AudioQueueDispose(AudioQueueRef inAQ, Boolean inImmediate);

#endif // MACSTUB_AUDIOTOOLBOX_H_
