/*
* This file is part of Wakanda software, licensed by 4D under
*  (i) the GNU General Public License version 3 (GNU GPL v3), or
*  (ii) the Affero General Public License version 3 (AGPL v3) or
*  (iii) a commercial license.
* This file remains the exclusive property of 4D and/or its licensors
* and is protected by national and international legislations.
* In any event, Licensee's compliance with the terms and conditions
* of the applicable license constitutes a prerequisite to any use of this file.
* Except as otherwise expressly stated in the applicable license,
* such license does not include any other license or rights on this file,
* 4D's and/or its licensors' trademarks and/or other proprietary rights.
* Consequently, no title, copyright or other proprietary rights
* other than those specified in the applicable license is granted.
*/
#include "headers4d.h"
#include "VRIAServerApplication.h"
#include "VRIAServerLogger.h"


USING_TOOLBOX_NAMESPACE


VRIARequestLogger::VRIARequestLogger()
: fLogger(NULL), fEnable(true)
{
}


VRIARequestLogger::VRIARequestLogger( VLog4jMsgFileLogger* inLogger)
: fLogger(inLogger), fEnable(true)
{
}


VRIARequestLogger::~VRIARequestLogger()
{
}


void VRIARequestLogger::Log( OsType inComponentID, void* inCDB4DBaseContext, sLONG inRequestNo, sLONG inRequestNbBytes, sLONG inReplyNbBytes, sLONG inElapsedTime)
{
	xbox_assert(false);	// RIA Server is not supposed to support 4th Dimension requests
}


void VRIARequestLogger::Log( OsType inComponentID, void* inCDB4DBaseContext, const VString& inMessage, sLONG inElapsedTime, bool inCleanString)
{
	if (fLogger != NULL)
	{
		StStringConverter<char> converter( VTC_StdLib_char);
		if (inCleanString)
		{
			VString tmp( inMessage);
			sLONG len = tmp.GetLength();
			UniChar* c = tmp.GetCPointerForWrite();
			if(c)
			{
				for(sLONG i=0;i<len;i++)
				{
					if(c[i]==0x0009)
					{
						c[i]=0x0020;
					}
					else
					{
					#if VERSIONWIN
						if(c[i]==0x000d)
						{
							if(i<len-1 && c[i+1]==0x000a)
							{
								c[i]=0x0020;
								c[i+1]=0x0020;
							}
						}
					#else
						if(c[i]==0x000d)
							c[i]=0x0020;
					#endif
					}
				}
			}
			Log( inComponentID, inCDB4DBaseContext, converter.ConvertString( tmp), inElapsedTime);
		}
		else
		{
			Log( inComponentID, inCDB4DBaseContext, converter.ConvertString( inMessage), inElapsedTime);
		}
	}
}


void VRIARequestLogger::Log( OsType inComponentID, void* inCDB4DBaseContext, const char* inMessage, sLONG inElapsedTime)
{
	if (fLogger != NULL)
	{
		OsType ctype[2] = { inComponentID, 0};
	#if SMALLENDIAN
		ByteSwapLong( &ctype[0]);
	#endif

		fLogger->Log( (const char*)&ctype, eL4JML_Information, inMessage, NULL);
		fLogger->Flush();
	}
}




StUseLogger::StUseLogger()
{
	fLogger = VRIAServerApplication::Get()->RetainLogger();
}


StUseLogger::~StUseLogger()
{
	if (fLogger != NULL)
	{
		XBOX::VLog4jMsgFileLogger *log4jLogger = dynamic_cast<XBOX::VLog4jMsgFileLogger*>(fLogger);
		if (log4jLogger != NULL)
			log4jLogger->Flush();

		fLogger->Release();
	}
}


void StUseLogger::Log( const XBOX::VString& inLoggerID, XBOX::ELog4jMessageLevel inLevel, const XBOX::VString& inMessage)
{
	if (fLogger != NULL)
	{
		VValueBag bag;
		ILoggerBagKeys::source.Set( &bag, inLoggerID);
		ILoggerBagKeys::level.Set( &bag, inLevel);
		ILoggerBagKeys::message.Set( &bag, inMessage);

		fLogger->LogBag( &bag);
	}
}


void StUseLogger::LogMessageFromErrorBase( const XBOX::VString& inLoggerID, XBOX::VErrorBase *inError)
{
	if (fLogger != NULL && inError != NULL)
	{
		// Append the component signature to the logger ID
		XBOX::VString loggerID( inLoggerID);
		loggerID.AppendUniChar( '.').AppendOsType( COMPONENT_FROM_VERROR( inError->GetError()));

		XBOX::VString errorDescription;	
		inError->GetErrorDescription( errorDescription);

		// Append the error code [XXXX] before the error description
		XBOX::VString message;
		message.AppendUniChar( '[').AppendLong( ERRCODE_FROM_VERROR( inError->GetError())).AppendUniCString( L"] ").AppendString( errorDescription);
		
		Log( loggerID,  XBOX::eL4JML_Error, message);
	}
}


void StUseLogger::LogMessagesFromErrorContext( const XBOX::VString& inLoggerID, XBOX::VErrorContext* inContext)
{
	if (fLogger != NULL && inContext != NULL)
	{
		for (std::vector<XBOX::VRefPtr<XBOX::VErrorBase> >::const_iterator iter = inContext->GetErrorStack().begin() ; iter != inContext->GetErrorStack().end() ; ++iter)
		{
			LogMessageFromErrorBase( inLoggerID, *iter);
		}
	}
}