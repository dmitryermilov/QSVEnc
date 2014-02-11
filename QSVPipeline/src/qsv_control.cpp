﻿/* ////////////////////////////////////////////////////////////////////////////// */
/*
//
//              INTEL CORPORATION PROPRIETARY INFORMATION
//  This software is supplied under the terms of a license  agreement or
//  nondisclosure agreement with Intel Corporation and may not be copied
//  or disclosed except in  accordance  with the terms of that agreement.
//        Copyright (c) 2005-2011 Intel Corporation. All Rights Reserved.
//
//
*/
#include <tchar.h>
#include <math.h>
#include <iostream>
#include <windows.h>
#include <process.h>
#include <emmintrin.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "sample_defs.h"
#include "sample_utils.h"
#include "mfxcommon.h"
#include "qsv_control.h"

#pragma warning( disable : 4748 )
CEncodeStatusInfo::CEncodeStatusInfo()
{
	m_nInputFrames = 0;
	m_nProcessedFramesNum = 0;
	m_nWrittenBytes = 0;
	m_nIDRCount = 0;
	m_nICount = 0;
	m_nPCount = 0;
	m_nBCount = 0;
	m_nIFrameSize = 0;
	m_nPFrameSize = 0;
	m_nBFrameSize = 0;
	m_nOutputFPSRate = 0;
	m_nOutputFPSScale = 0;
	m_pStrLog = NULL;
	m_bStdErrWriteToConsole = true;
}

void CEncodeStatusInfo::Init(mfxU32 outputFPSRate, mfxU32 outputFPSScale, mfxU32 totalOutputFrames, TCHAR *pStrLog) {
	m_nOutputFPSRate = outputFPSRate;
	m_nOutputFPSScale = outputFPSScale;
	m_nTotalOutFrames = totalOutputFrames;
	m_pStrLog = pStrLog;
	DWORD mode = 0;
	m_bStdErrWriteToConsole = 0 != GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode); //stderrの出力先がコンソールかどうか
}

void CEncodeStatusInfo::SetStart() {
	m_tmStart = timeGetTime();
}

CEncodingThread::CEncodingThread()
{
	m_nFrameBuffer = 0;
	m_thEncode = NULL;
	m_thSub = NULL;
	m_bthForceAbort = FALSE;
	m_bthSubAbort = FALSE;
	m_nFrameSet = 0;
	m_nFrameGet = 0;
	m_InputBuf = NULL;
	m_bInit = false;
}

CEncodingThread::~CEncodingThread()
{
	Close();
}

mfxStatus CEncodingThread::Init(mfxU16 bufferSize) {
	Close();

	m_nFrameBuffer = bufferSize;
	m_InputBuf = (sInputBufSys *)_mm_malloc(m_nFrameBuffer * sizeof(sInputBufSys), 64);
	MSDK_CHECK_POINTER(m_InputBuf, MFX_ERR_NULL_PTR);
	memset(m_InputBuf, 0, m_nFrameBuffer * sizeof(sInputBufSys));

	for (mfxU32 i = 0; i < m_nFrameBuffer; i++) {
		m_InputBuf[i].heInputDone  = CreateEvent(NULL, FALSE, FALSE, NULL);
		MSDK_CHECK_ERROR(m_InputBuf[i].heInputDone, NULL, MFX_ERR_INVALID_HANDLE);

		m_InputBuf[i].heSubStart  = CreateEvent(NULL, FALSE, FALSE, NULL);
		MSDK_CHECK_ERROR(m_InputBuf[i].heSubStart, NULL, MFX_ERR_INVALID_HANDLE);

		m_InputBuf[i].heInputStart = CreateEvent(NULL, FALSE, FALSE, NULL);
		MSDK_CHECK_ERROR(m_InputBuf[i].heInputStart, NULL, MFX_ERR_INVALID_HANDLE);
	}
	m_bInit = true;
	return MFX_ERR_NONE;
}

mfxStatus CEncodingThread::RunEncFuncbyThread(unsigned (__stdcall * func) (void *), void *pClass, DWORD_PTR threadAffinityMask) {
	MSDK_CHECK_ERROR(m_bInit, false, MFX_ERR_NOT_INITIALIZED);

	m_thEncode = (HANDLE)_beginthreadex(NULL, NULL, func, pClass, FALSE, NULL);
	MSDK_CHECK_ERROR(m_thEncode, NULL, MFX_ERR_INVALID_HANDLE);

	if (threadAffinityMask)
		SetThreadAffinityMask(m_thEncode, threadAffinityMask);

	return MFX_ERR_NONE;
}

mfxStatus CEncodingThread::RunSubFuncbyThread(unsigned (__stdcall * func) (void *), void *pClass, DWORD_PTR threadAffinityMask) {
	MSDK_CHECK_ERROR(m_bInit, false, MFX_ERR_NOT_INITIALIZED);

	m_thSub = (HANDLE)_beginthreadex(NULL, NULL, func, pClass, FALSE, NULL);
	MSDK_CHECK_ERROR(m_thEncode, NULL, MFX_ERR_INVALID_HANDLE);

	if (threadAffinityMask)
		SetThreadAffinityMask(m_thSub, threadAffinityMask);

	return MFX_ERR_NONE;
}

//終了を待機する
mfxStatus CEncodingThread::WaitToFinish(mfxStatus sts) {
	MSDK_CHECK_ERROR(m_bInit, false, MFX_ERR_NOT_INITIALIZED);
	MSDK_CHECK_ERROR(m_thEncode, NULL, MFX_ERR_INVALID_HANDLE);
	//最後のLoadNextFrameの結果をm_stsThreadにセットし、RunEncodeに知らせる
	m_stsThread = sts;
	//読み込み終了(MFX_ERR_MORE_DATA)ではなく、エラーや中断だった場合、
	//直ちに終了する
	if (sts != MFX_ERR_MORE_DATA) {
		InterlockedIncrement((DWORD*)&m_bthForceAbort); //m_bthForceAbort = TRUE;
		InterlockedIncrement((DWORD*)&m_bthSubAbort); //m_bthSubAbort = TRUE;
	//	if (m_InputBuf) {
	//		for (mfxU32 i = 0; i < m_nFrameBuffer; i++) {
	//			SetEvent(m_InputBuf[i].heInputDone);
	//			SetEvent(m_InputBuf[i].heSubStart);
	//		}
	//	}
	}
	//RunEncodeの終了を待つ
	WaitForSingleObject(m_thEncode, INFINITE);
	CloseHandle(m_thEncode);
	m_thEncode = NULL;
	return MFX_ERR_NONE;
}

void CEncodingThread::Close()
{
	if (m_thEncode) {
		WaitForSingleObject(m_thEncode, INFINITE);
		CloseHandle(m_thEncode);
		m_thEncode = NULL;
	}
	if (m_thSub) {
		InterlockedIncrement((DWORD*)&m_bthSubAbort);
		for (mfxU32 i = 0; i < m_nFrameBuffer; i++)
			SetEvent(m_InputBuf[i].heSubStart);
		WaitForSingleObject(m_thSub, INFINITE);
		CloseHandle(m_thSub);
		m_thSub = NULL;
	}
	if (m_InputBuf) {
		for (mfxU32 i = 0; i < m_nFrameBuffer; i++) {
			if (m_InputBuf[i].heInputDone)
				CloseHandle(m_InputBuf[i].heInputDone);
			if (m_InputBuf[i].heSubStart)
				CloseHandle(m_InputBuf[i].heSubStart);
			if (m_InputBuf[i].heInputStart)
				CloseHandle(m_InputBuf[i].heInputStart);
		}
		_mm_free(m_InputBuf);
		m_InputBuf = NULL;
	}
	m_nFrameBuffer = 0;
	m_nFrameSet = 0;
	m_nFrameGet = 0;
	m_bthSubAbort = FALSE;
	m_bthForceAbort = FALSE;
	m_stsThread = MFX_ERR_NONE;
	m_bInit = false;
}