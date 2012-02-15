/*
 * CallLogReader.h
 *
 *  Created on: 20/mag/2010
 *      Author: Giovanna
 */

#ifndef CALLLOGREADER_H_
#define CALLLOGREADER_H_

#include <F32FILE.H>
#include <LOGVIEW.H>
#include <logcli.h>

// platform macros are defined into epoc32/tools/e32plat.pm
// if platform is symbian^3, __SERIES60_3X__ is not defined
// except than Symbian^3, KLogMaxDirectionLength is defined into <logwrap.h>
#ifndef __SERIES60_3X__
const TInt KLogMaxDirectionLength = 64;
#endif

enum TCallDirection
	{
	EDirIn,
	EDirOut,
	EDirMissed,
	EDirFetched
	};

class MCallLogCallBack
{
    public:
        virtual void HandleCallLogEventL(TInt aDirection,const CLogEvent& event) = 0;
        virtual void CallLogProcessed(TInt aError) = 0;
};

enum TCallLogReaderState
    {
        ECreatingView,
        EReadingEntries
    };
 
 
class CCallLogReader: public CActive
{
 
    public: // constructors and destructor	
        static CCallLogReader* NewL(MCallLogCallBack& aCallBack, RFs& aFs);
        static CCallLogReader* NewLC(MCallLogCallBack& aCallBack, RFs& aFs);
        ~CCallLogReader();
 
    public: // from CActive
        void DoCancel();
        void RunL();
        
        void ReadCallLogL();
 
    private: // constructors
        CCallLogReader(MCallLogCallBack& aCallBack, RFs& aFs);
        void ConstructL();
        void DoneReadingL(TInt aError);
 
    private: // data
        TCallLogReaderState iEngineState;
        CLogClient*         iLogClient; 
        CLogViewEvent*      iLogView;
        CLogFilter*         iLogFilter;
        MCallLogCallBack&   iCallBack;
        RFs&                iFs;
        
        TBuf<KLogMaxDirectionLength>  	iDirOut;
        TBuf<KLogMaxDirectionLength>	iDirMissed;
        TBuf<KLogMaxDirectionLength>	iDirFetched;
        TBuf<KLogMaxDirectionLength>	iDirIn;
        //TBuf<KLogMaxDirectionLength>	iOutAlt;
        //TBuf<KLogMaxDirectionLength>	iInAlt;
            
};
#endif /* CALLLOGREADER_H_ */
