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
#include "VRIAServerSolution.h"
#include "VRIAServerProject.h"
#include "VRIAServerTools.h"
#include "VRIAServerApplication.h"
#include "VRIAServerProjectContext.h"
#include "DB4D/Headers/DB4D.h"
#include "VRPCCatalog.h"
#include "VRIAServerLogger.h"
#include "VJSApplication.h"
#include "VRIAServerJSAPI.h"
#include "VJSSolution.h"
#include "VRIAServerJSContextMgr.h"
#include "VRIAServerHTTPSession.h"
#include "JavaScript/Sources/VJSJSON.h"
#include "Language Syntax/CLanguageSyntax.h"
#include "VRIAServerJSCore.h"
#include "VJSRPCServiceCore.h"

USING_TOOLBOX_NAMESPACE



// ----------------------------------------------------------------------------



VRIAServerJSContextMgr::VRIAServerJSContextMgr()
{
}


VRIAServerJSContextMgr::~VRIAServerJSContextMgr()
{
	xbox_assert(fSetOfPool.empty());
}


VJSContextPool* VRIAServerJSContextMgr::CreateJSContextPool( VError& outError, IJSContextPoolDelegate* inDelegate)
{
	outError = VE_OK;
	VJSContextPool *pool = new VJSContextPool( this, inDelegate);

	if (pool != NULL)
	{
		_RegisterPool( pool);
	}
	else
	{
		outError = ThrowError( VE_MEMORY_FULL);
	}
	return pool;
}


void VRIAServerJSContextMgr::GarbageCollect()
{
	if (fSetOfPoolMutex.Lock())
	{
		for (SetOfPool_iter iter = fSetOfPool.begin() ; iter != fSetOfPool.end() ; ++iter)
		{
			(*iter)->GarbageCollect();
		}
		fSetOfPoolMutex.Unlock();
	}
}


VJSGlobalContext* VRIAServerJSContextMgr::RetainJSContext( VError& outError, const VJSContext& inParentContext, bool inReusable)
{
	VJSGlobalContext *result = NULL;
	outError = VE_OK;

	if (inParentContext != NULL)
	{
		VRIAJSRuntimeContext *runtimeContext = VRIAJSRuntimeContext::GetFromJSContext( inParentContext);
		if (runtimeContext != NULL)
		{
			VRIAServerProject *application = runtimeContext->GetRootApplication();
			if (application != NULL)
			{
				result = application->RetainJSContext( outError, inReusable, NULL);
			}
			else
			{
				result = VRIAServerApplication::Get()->RetainJSContext( outError, inReusable);
			}
		}
	}

	return result;
}


VError VRIAServerJSContextMgr::ReleaseJSContext( VJSGlobalContext* inContext)
{
	VError err = VE_OK;

	if (inContext != NULL)
	{
		VJSContext jsContext( inContext);
		VRIAJSRuntimeContext *runtimeContext = VRIAJSRuntimeContext::GetFromJSContext( jsContext);
		if (runtimeContext != NULL)
		{
			VRIAServerProject *application = runtimeContext->GetRootApplication();
			if (application != NULL)
			{
				application->ReleaseJSContext( inContext, NULL);
			}
			else
			{
				err = VRIAServerApplication::Get()->ReleaseJSContext( inContext);
			}
		}
	}

	return err;
}



void VRIAServerJSContextMgr::_RegisterPool( VJSContextPool *inPool)
{
	if (inPool == NULL)
		return;

	if (fSetOfPoolMutex.Lock())
	{
		fSetOfPool.insert( inPool);
		fSetOfPoolMutex.Unlock();
	}

	return;
}


void VRIAServerJSContextMgr::_UnRegisterPool( VJSContextPool *inPool)
{
	if (inPool == NULL)
		return;

	if (fSetOfPoolMutex.Lock())
	{
		SetOfPool_iter found = fSetOfPool.find( inPool);
		if (found != fSetOfPool.end())
		{
			fSetOfPool.erase( found);
		}
		fSetOfPoolMutex.Unlock();
	}
}



// ----------------------------------------------------------------------------


VRIAJSRuntimeContext::VRIAJSRuntimeContext()
: fRootApplication(NULL), fCurrentUAGSession(NULL)
{
	fGlobalContext = NULL;
}


VRIAJSRuntimeContext::VRIAJSRuntimeContext( VRIAServerProject* inRootApplication)
: fRootApplication(inRootApplication), fCurrentUAGSession(NULL)
{
	fGlobalContext = NULL;
}


VRIAJSRuntimeContext::~VRIAJSRuntimeContext()
{
	fContextMap.clear();
	QuickReleaseRefCountable( fCurrentUAGSession);
}


VRIAServerProject* VRIAJSRuntimeContext::GetRootApplication() const
{
	return fRootApplication;
}


VRIAServerSolution* VRIAJSRuntimeContext::GetRootSolution() const
{
	if (fRootApplication != NULL)
		return fRootApplication->GetSolution();

	return NULL;
}


VError VRIAJSRuntimeContext::InitializeJSContext( VJSGlobalContext* inContext, VRIAServerProject *inRootApplication)
{	
	if (inContext == NULL)
		return VE_INVALID_PARAMETER;

	VError err = VE_OK;
	VJSContext jsContext( inContext);
	VRIAJSRuntimeContext *runtimeContext = GetFromJSContext( jsContext);
	if (testAssert(runtimeContext == NULL))
	{
		runtimeContext = new VRIAJSRuntimeContext( inRootApplication);
		if (runtimeContext != NULL)
		{
			// Attach this runtime context
			if (runtimeContext->_AttachToJSContext( jsContext))
			{
				runtimeContext->fGlobalContext = inContext;
				// Attach the db4d context
				CDB4DManager *db4d = VRIAServerApplication::Get()->GetComponentDB4D();
				if (db4d != NULL)
				{
					VUUID uuid( true);
					CDB4DContext *db4dContext = db4d->RetainOrCreateContext( uuid, NULL, inContext);
					if (db4dContext != NULL)
					{
						db4d->InitializeJSContext( inContext, db4dContext);
						ReleaseRefCountable( &db4dContext);
					}
					else
					{
						err = VE_UNKNOWN_ERROR;
					}
				}
				else
				{
					err = ThrowError( VE_RIA_DB4D_COMPONENT_NOT_FOUND);
				}

				if (err == VE_OK && runtimeContext->fRootApplication != NULL)
				{
					err = runtimeContext->AttachSolution( jsContext, runtimeContext->fRootApplication->GetSolution());
				}
			}
			else
			{
				err = VE_UNKNOWN_ERROR;
			}

			ReleaseRefCountable( &runtimeContext);
		}
		else
		{
			err = ThrowError( VE_MEMORY_FULL);
		}
	}
	else
	{
		err = VE_UNKNOWN_ERROR;
	}

	if (err != VE_OK)
		err = ThrowError( VE_RIA_JS_CANNOT_INITIALIZE_CONTEXT);

	return err;
}


VError VRIAJSRuntimeContext::UninitializeJSContext( VJSGlobalContext* inContext)
{
	if (inContext == NULL)
		return VE_INVALID_PARAMETER;
	
	VError err = VE_OK;
	VJSContext jsContext( inContext);
	VRIAJSRuntimeContext *runtimeContext = GetFromJSContext( jsContext);
	if (testAssert(runtimeContext != NULL))
	{
		if (runtimeContext->GetRootSolution() != NULL)
		{
			runtimeContext->DetachSolution( jsContext, runtimeContext->GetRootSolution());
		}

		assert(runtimeContext->fContextMap.size() == 0);
		
		// Detach the db4d context
		CDB4DManager *db4d = VRIAServerApplication::Get()->GetComponentDB4D();
		if (db4d != NULL)
		{
			db4d->UninitializeJSContext( inContext);
		}
		else
		{
			err = ThrowError( VE_RIA_DB4D_COMPONENT_NOT_FOUND);
		}

		// Detach the runtime context
		runtimeContext->_DetachFromJSContext( jsContext);
	}
	else
	{
		err =  VE_UNKNOWN_ERROR;
	}

	if (err != VE_OK)
		err = ThrowError( VE_RIA_JS_CANNOT_UNINITIALIZE_CONTEXT);
	
	return err;
}


XBOX::VError VRIAJSRuntimeContext::AttachSolution( const XBOX::VJSContext& inContext, VRIAServerSolution *inSolution)
{
	if (inSolution == NULL)
		return VE_INVALID_PARAMETER;

	VRIAJSRuntimeContext *runtimeContext = GetFromJSContext( inContext);
	if (runtimeContext != this)
		return VE_UNKNOWN_ERROR;

	// For each application, create an application context and a database context if needed.
	VError err = VE_OK;
	VectorOfApplication appCollection;
	inSolution->GetApplications( appCollection);

	for (VectorOfApplication_iter iter = appCollection.begin() ; iter != appCollection.end() ; ++iter)
	{
		if (!iter->IsNull())
		{
			if (fContextMap.find( *iter) == fContextMap.end())
			{
				// Create the application context
				VRIAContext *riaContext = (*iter)->RetainNewContext( err);
				if (err == VE_OK && riaContext != NULL)
				{
					// Register the application context
					fContextMap[*iter] = VRefPtr<VRIAContext>(riaContext);

					CDB4DBase *base = (*iter)->RetainDatabase( riaContext);
					if (base != NULL)
					{
						CDB4DManager *db4d = VRIAServerApplication::Get()->GetComponentDB4D();
						if (db4d != NULL)
						{
							CDB4DContext *db4dContext = db4d->GetDB4DContextFromJSContext( inContext);
							if (db4dContext != NULL)
							{
								// Create and register the database context
								CDB4DBaseContext *baseContext = db4dContext->RetainDataBaseContext( base, true, false);
								if (baseContext != NULL)
									riaContext->SetBaseContext( baseContext);
								else
									err = VE_UNKNOWN_ERROR;
							}
							else
							{
								err = VE_UNKNOWN_ERROR;
							}
						}
						else
						{
							err = VE_RIA_DB4D_COMPONENT_NOT_FOUND;
						}

						base->Release();
					}	
				}
				ReleaseRefCountable( &riaContext);
			}
		}
	}

	return err;
}


XBOX::VError VRIAJSRuntimeContext::DetachSolution( const XBOX::VJSContext& inContext, VRIAServerSolution *inSolution)
{
	if (inContext == NULL || inSolution == NULL)
		return VE_INVALID_PARAMETER;

	VRIAJSRuntimeContext *runtimeContext = GetFromJSContext( inContext);
	if (runtimeContext != this)
		return VE_UNKNOWN_ERROR;

	// For each application, release the application context and the database context if needed.
	VError err = VE_OK;
	VectorOfApplication appCollection;
	inSolution->GetApplications( appCollection);

	for (VectorOfApplication_iter iter = appCollection.begin() ; (iter != appCollection.end()) && (err == VE_OK) ; ++iter)
	{
		if (!iter->IsNull())
		{
			MapOfRIAContext_iter found = fContextMap.find( *iter);
			if (found != fContextMap.end())
			{
				fContextMap.erase( found);
			}
		}
	}

	return VE_OK;
}


XBOX::VJSSessionStorageObject* VRIAJSRuntimeContext::GetSessionStorageObject()
{
	if (fCurrentUAGSession != NULL)
		return fCurrentUAGSession->GetStorageObject();
	else
		return NULL;
}

CUAGSession* VRIAJSRuntimeContext::RetainUAGSession()
{
	return RetainRefCountable(fCurrentUAGSession);
}


CDB4DContext* VRIAJSRuntimeContext::RetainDB4DContext(VRIAServerProject* inApplication)
{
	CDB4DContext* result = NULL;
	VRIAContext* riacontext = GetApplicationContext(inApplication);
	if (riacontext != NULL && riacontext->GetBaseContext() != NULL)
	{
		result = RetainRefCountable(riacontext->GetBaseContext()->GetContextOwner());
	}
	return result;
}


XBOX::VError VRIAJSRuntimeContext::SetUAGSession(CUAGSession* inSession, bool addSession)
{
	VRIAServerProject *application = GetRootApplication();

	if (fCurrentUAGSession != NULL)
	{
		if (fCurrentUAGSession->IsDefault() && !fCurrentUAGSession->IsEmpty() && inSession != NULL)
		{
			inSession->SetStorageObject(fCurrentUAGSession->GetStorageObject());
		}
	}

	CopyRefCountable(&fCurrentUAGSession, inSession);
	if (addSession)
	{
		VRIAHTTPSessionManager* sessionMgr = fRootApplication->RetainSessionMgr();
		if (sessionMgr != NULL)
		{
			sessionMgr->AddSession(fCurrentUAGSession);
			sessionMgr->Release();
		}
	}

	VJSContext jscontext(fGlobalContext);
	VJSSessionStorageObject* storage = GetSessionStorageObject();
	if (storage == NULL)
	{
		VJSValue value( jscontext);
		value.SetNull();
		jscontext.GetGlobalObject().SetProperty( "sessionStorage", value);
	}
	else
		jscontext.GetGlobalObject().SetProperty( "sessionStorage", VJSStorageClass::CreateInstance(jscontext, storage));

	CDB4DContext* basecontext = RetainDB4DContext(application);
	if (basecontext != NULL)
	{
		VUUID userID;
		userID.SetNull(true);
		basecontext->SetCurrentUser(userID, inSession);
		basecontext->Release();
	}

	return VE_OK;
}



VError VRIAJSRuntimeContext::RegisterApplicationContext( VRIAContext* inContext)
{
	VError err = VE_OK;

	if (inContext != NULL && inContext->GetApplication() != NULL)
	{
		MapOfRIAContext_iter found = fContextMap.find( inContext->GetApplication());
		if (found == fContextMap.end())
		{
			fContextMap[inContext->GetApplication()] = VRefPtr<VRIAContext>(inContext);
		}
	}
	return err;
}


VRIAContext* VRIAJSRuntimeContext::GetApplicationContext( VRIAServerProject* inApplication) const
{
	VRIAContext *context = NULL;

	if (inApplication != NULL)
	{
		MapOfRIAContext_citer found = fContextMap.find( inApplication);
		if (found != fContextMap.end())
		{
			context = found->second.Get();
		}
	}
	return context;
}


VRIAJSRuntimeContext* VRIAJSRuntimeContext::GetFromJSContext( const XBOX::VJSContext& inContext)
{
	return static_cast<VRIAJSRuntimeContext*>( inContext.GetGlobalObjectPrivateInstance()->GetSpecific( 'riax'));
}


VRIAContext* VRIAJSRuntimeContext::GetApplicationContextFromJSContext( const XBOX::VJSContext& inContext, VRIAServerProject* inApplication)
{
	VRIAJSRuntimeContext *rtContext = GetFromJSContext( inContext);
	return (rtContext != NULL) ? rtContext->GetApplicationContext( inApplication) : NULL;
}


VRIAJSRuntimeContext* VRIAJSRuntimeContext::GetFromJSGlobalObject( const XBOX::VJSGlobalObject* inGlobalObject)
{
	if (inGlobalObject != NULL)
		return static_cast<VRIAJSRuntimeContext*>( inGlobalObject->GetSpecific( 'riax'));

	return NULL;
}


bool VRIAJSRuntimeContext::_AttachToJSContext( XBOX::VJSContext& inContext)
{
	bool done = inContext.GetGlobalObjectPrivateInstance()->SetSpecific( 'riax', this, VJSSpecifics::DestructorReleaseVObject);
	if (done)
	{
		Retain();
	}
	return done;
}


bool VRIAJSRuntimeContext::_DetachFromJSContext( XBOX::VJSContext& inContext)
{
	assert( VRIAJSRuntimeContext::GetFromJSContext( inContext) == this);
	return inContext.GetGlobalObjectPrivateInstance()->SetSpecific( 'riax', NULL, VJSSpecifics::DestructorReleaseVObject);
}



// ----------------------------------------------------------------------------



class VJSContextPoolSpecific : public XBOX::VObject
{
public:
	VJSContextPoolSpecific() {;}
	virtual ~VJSContextPoolSpecific() {;}
};



class VJSContextInfo : public XBOX::VObject
{
public:

	VJSContextInfo() : fGlobalObject(NULL), fDebuggerActive(false), fReusable(false) {;}

	VJSContextInfo( const VJSContextInfo& inSource) : fGlobalObject(inSource.fGlobalObject), fDebuggerActive(inSource.fDebuggerActive), fReusable( inSource.fReusable) {;}

	virtual ~VJSContextInfo() {;}

	VJSContextInfo& operator = ( const VJSContextInfo& inSource)
	{
		fGlobalObject = inSource.fGlobalObject;
		fDebuggerActive = inSource.fDebuggerActive;
		fReusable = inSource.fReusable;
		return *this;
	}

	void				SetGlobalObject( VJSGlobalObject* inGlobalObject) { fGlobalObject = inGlobalObject; }
	VJSGlobalObject*	GetGlobalObject() const { return fGlobalObject; }

	void				SetDebuggerActive( bool inDebuggerActive) { fDebuggerActive = inDebuggerActive; }
	bool				IsDebuggerActive() const { return fDebuggerActive; }

	void				SetReusable( bool inReusable) { fReusable = inReusable; }
	bool				IsReusable() const { return fReusable; }

private:

	XBOX::VJSGlobalObject*	fGlobalObject;
	bool					fDebuggerActive;	// state of the debugger when context was created
	bool					fReusable;
};



const size_t kREUSABLE_JSCONTEXT_MAX_COUNT = 50;


VJSContextPool::VJSContextPool()
:  fManager(NULL), fEnabled(true), fContextReusingEnabled(true), fDelegate(NULL), fNoUsedContextEvent(NULL), fReusableContextCount(0)
{
}


VJSContextPool::VJSContextPool( VRIAServerJSContextMgr *inMgr, IJSContextPoolDelegate* inDelegate)
: fManager(inMgr), fEnabled(true), fContextReusingEnabled(true), fDelegate(inDelegate), fNoUsedContextEvent(NULL), fReusableContextCount(0)
{
	xbox_assert(fManager != NULL);
}


VJSContextPool::~VJSContextPool()
{
	if (fManager != NULL)
		fManager->_UnRegisterPool( this);

	xbox_assert( (fUsedContexts.size() == 0) && (fUnusedContexts.size() == 0) );
	
	if (fNoUsedContextEvent != NULL)
	{
		if (fNoUsedContextEvent->Unlock())
			QuickReleaseRefCountable( fNoUsedContextEvent);
	}

	fRequiredScripts.clear();
}


VJSGlobalContext* VJSContextPool::RetainNewContext( IJSRuntimeDelegate* inDelegate)
{
	// Register our classes
	_InitGlobalClasses();

	VJSGlobalContext *globalContext = VJSGlobalContext::Create( inDelegate);

	return globalContext;
}


VJSGlobalContext* VJSContextPool::RetainContext( VError& outError, bool inReusable, XBOX::VJSGlobalContext* inPreferedContext)
{
	outError = VE_OK;

	VJSGlobalContext *globalContext = NULL;

	if (fEnabled)
	{
		if (inReusable && fContextReusingEnabled)
		{
			// Try to reuse an existing pooled context
			if (fPoolMutex.Lock())
			{
				MapOfJSContext_iter iter = fUnusedContexts.end();
				
				// Try to reuse the prefered context
				if (inPreferedContext != NULL)
					iter = fUnusedContexts.find( inPreferedContext);
				
				if (iter == fUnusedContexts.end())
					iter = fUnusedContexts.begin();

				while ((globalContext == NULL) && (iter != fUnusedContexts.end()))
				{
					xbox_assert(iter->second->IsReusable());

					VJSGlobalObject *globalObject = iter->second->GetGlobalObject();
					if (globalObject != NULL && globalObject->IsIncludedFilesHaveBeenChanged())
					{
						// If some included files have been changed, the context is invalid and must not be reused
						VJSGlobalContext *gContext = iter->first;
						
						// Remove this context from unused contexts pool
						--fReusableContextCount;
						delete iter->second;
						fUnusedContexts.erase( iter);

						// Release this context
						globalObject->GarbageCollect();
						_ReleaseContext( gContext);

						iter = fUnusedContexts.begin();
					}
					else
					{
						// The context becomes used
						globalContext = iter->first;
						fUsedContexts[globalContext] = iter->second;
						fUnusedContexts.erase( iter);
					}
				}
				fPoolMutex.Unlock();
			}
		}

		if (globalContext == NULL)
		{
			// Create a new context
			globalContext = _RetainNewContext( outError);
			if (globalContext != NULL)
			{
				// Reference the context in the pool as used context
				if (fPoolMutex.Lock())
				{
					VJSContextInfo *info = new VJSContextInfo();
					if  (info != NULL)
					{
						VJSContext jsContext( globalContext);
						info->SetGlobalObject( jsContext.GetGlobalObjectPrivateInstance());
						info->SetDebuggerActive( VJSGlobalContext::IsDebuggerActive());

						if (inReusable && fContextReusingEnabled && (fReusableContextCount < kREUSABLE_JSCONTEXT_MAX_COUNT))
						{
							info->SetReusable( true);
							++fReusableContextCount;
						}
						else
						{
							info->SetReusable( false);
						}

						fUsedContexts[globalContext] = info;
					}
					else
					{
						outError = ThrowError( VE_MEMORY_FULL);
					}					

					fPoolMutex.Unlock();
				}
			}
		}
	}
	return globalContext;
}


VError VJSContextPool::ReleaseContext( VJSGlobalContext* inContext)
{
	VError err = VE_OK;

	if (inContext != NULL)
	{
		bool isReusable = false;
	
		if (fPoolMutex.Lock())
		{
			MapOfJSContext_iter found = fUsedContexts.find( inContext);
			if (found != fUsedContexts.end())
			{
				if (fContextReusingEnabled && found->second->IsReusable())
				{
					VJSGlobalObject *globalObject = found->second->GetGlobalObject();
					if (globalObject != NULL && globalObject->IsIncludedFilesHaveBeenChanged())
					{
						// If some included files have been changed, the context is invalid and must not be reused
						isReusable = false;
					}
					else
					{
						fUnusedContexts[inContext] = found->second;
						isReusable = true;
					}
				}

				// The context becomes unused
				if (!isReusable)
				{
					VJSGlobalObject *globalObject = found->second->GetGlobalObject();
					if (globalObject != NULL)
						globalObject->GarbageCollect();

					if (found->second->IsReusable())
						--fReusableContextCount;
					
					delete found->second;
					found->second = NULL;
				}

				fUsedContexts.erase( found);

				if (fUsedContexts.size() == 0)
				{
					if (fNoUsedContextEvenMutex.Lock())
					{
						if (fNoUsedContextEvent != NULL)
						{
							if (fNoUsedContextEvent->Unlock())
								ReleaseRefCountable( &fNoUsedContextEvent);
						}
						fNoUsedContextEvenMutex.Unlock();
					}
				}
			}
			fPoolMutex.Unlock();
		}

		if (!isReusable)
			err = _ReleaseContext( inContext);
	}
	return err;
}


bool VJSContextPool::SetEnabled( bool inEnabled)
{
	bool previousState = fEnabled;
	fEnabled = inEnabled;
	return previousState;
}


bool VJSContextPool::IsEnabled() const
{
	return fEnabled;
}


void VJSContextPool::SetContextReusingEnabled( bool inEnabled)
{
	fContextReusingEnabled = inEnabled;
}


bool VJSContextPool::IsContextReusingEnabled() const
{
	return fContextReusingEnabled;
}


VSyncEvent* VJSContextPool::WaitForNumberOfUsedContextEqualZero()
{
	VSyncEvent *syncEvent = NULL;

	if (fPoolMutex.Lock())
	{
		if (fUsedContexts.size() > 0)
		{
			if (fNoUsedContextEvenMutex.Lock())
			{
				if (fNoUsedContextEvent == NULL)
					fNoUsedContextEvent = new VSyncEvent();

				syncEvent = RetainRefCountable( fNoUsedContextEvent);

				fNoUsedContextEvenMutex.Unlock();
			}
		}
		fPoolMutex.Unlock();
	}
	return syncEvent;
}


VError VJSContextPool::Clear()
{
	VError err = VE_OK;

	bool savedEnabledState = fEnabled;	// save the enabled state

	fEnabled = false;

	VSyncEvent *syncEvent = WaitForNumberOfUsedContextEqualZero();
	if (syncEvent != NULL)
	{
		syncEvent->Lock();
		syncEvent->Release();
	}

	if (fPoolMutex.Lock())
	{
		for (MapOfJSContext_iter iter = fUnusedContexts.begin() ; iter != fUnusedContexts.end() ; ++iter)
		{
			xbox_assert(iter->second->IsReusable());

			VJSGlobalObject *globalObject = iter->second->GetGlobalObject();
			if (globalObject != NULL)
				globalObject->GarbageCollect();

			_ReleaseContext( iter->first);

			delete iter->second;
			--fReusableContextCount;
		}
		
		xbox_assert(fReusableContextCount == 0);
		fUnusedContexts.clear();

		fPoolMutex.Unlock();
	}

	fEnabled = savedEnabledState;	// restore the enabled state

	return err;
}


void VJSContextPool::AppendRequiredScript( const VFilePath& inPath)
{
	if (fRequiredScriptsMutex.Lock())
	{
		std::vector<VFilePath>::iterator found = std::find( fRequiredScripts.begin(), fRequiredScripts.end(), inPath);
		if (found == fRequiredScripts.end())
			fRequiredScripts.push_back( inPath);

		fRequiredScriptsMutex.Unlock();
	}
}


void VJSContextPool::RemoveRequiredScript( const VFilePath& inPath)
{
	if (fRequiredScriptsMutex.Lock())
	{
		std::vector<VFilePath>::iterator found = std::find( fRequiredScripts.begin(), fRequiredScripts.end(), inPath);
		if (found != fRequiredScripts.end())
			fRequiredScripts.erase( found);

		fRequiredScriptsMutex.Unlock();
	}
}


void VJSContextPool::GarbageCollect()
{
	if (fPoolMutex.Lock())
	{
		for (MapOfJSContext_iter iter = fUnusedContexts.begin() ; iter != fUnusedContexts.end() ; ++iter)
		{
			if  (iter->second != NULL)
			{
				VJSGlobalObject *globalObject = iter->second->GetGlobalObject();
				if (globalObject != NULL)
				{
					globalObject->GarbageCollect();
				}
			}
		}

		for (MapOfJSContext_iter iter = fUsedContexts.begin() ; iter != fUsedContexts.end() ; ++iter)
		{
			if  (iter->second != NULL)
			{
				VJSGlobalObject *globalObject = iter->second->GetGlobalObject();
				if (globalObject != NULL)
				{
					globalObject->GarbageCollect();
				}
			}
		}

		fPoolMutex.Unlock();
	}
}


bool VJSContextPool::Init()
{
	_InitGlobalClasses();
	return true;
}


XBOX::VFolder* VJSContextPool::RetainScriptsFolder()
{
	return NULL;
}


XBOX::VProgressIndicator* VJSContextPool::CreateProgressIndicator( const XBOX::VString& inTitle)
{
	return NULL;
}


VJSGlobalContext* VJSContextPool::_RetainNewContext( VError& outError)
{
	outError = VE_OK;

	IJSRuntimeDelegate *runtimeDelegate = (fDelegate != NULL) ? fDelegate->GetRuntimeDelegate() : NULL;

	if (runtimeDelegate == NULL)
		runtimeDelegate = this;

	VJSGlobalContext *globalContext = RetainNewContext( runtimeDelegate);
	if (globalContext != NULL)
	{
		// Evaluating required scripts
		if (fRequiredScriptsMutex.Lock()) // attention possibilite importante d'engorgement, voir L.R
		{
			for (std::vector<VFilePath>::iterator iter = fRequiredScripts.begin() ; iter != fRequiredScripts.end() ; ++iter)
			{
				VFile *script = new VFile( *iter);
				if (script != NULL)
				{
					if (script->Exists())
					{
						VJSContext jsContext( globalContext);
						VJSGlobalObject *globalObject = jsContext.GetGlobalObjectPrivateInstance();
						if (testAssert(globalObject != NULL))
							globalObject->RegisterIncludedFile( script);	// sc 17/01/2011 to invalid the context when the entity Entity Model script is modified

						globalContext->EvaluateScript( script, NULL);
					}
				}
				ReleaseRefCountable( &script);
			}
			fRequiredScriptsMutex.Unlock();
		}

		CDB4DManager *db4d = VRIAServerApplication::Get()->GetComponentDB4D();
		if (db4d != NULL)
		{
			std::vector<VFilePath> db4dRequired;
			db4d->GetStaticRequiredJSFiles(db4dRequired);
			for (std::vector<VFilePath>::iterator iter = db4dRequired.begin() ; iter != db4dRequired.end() ; ++iter)
			{
				VFile *script = new VFile( *iter);
				if (script != NULL)
				{
					if (script->Exists())
					{
						VJSContext jsContext( globalContext);
						VJSGlobalObject *globalObject = jsContext.GetGlobalObjectPrivateInstance();
						if (testAssert(globalObject != NULL))
							globalObject->RegisterIncludedFile( script);

						globalContext->EvaluateScript( script, NULL);
					}
				}
				ReleaseRefCountable( &script);
			}
		}

		if (fDelegate != NULL)
		{
			outError = fDelegate->InitializeJSContext( globalContext);
		}
	}
	else
	{
		outError = ThrowError( VE_RIA_JS_CANNOT_CREATE_CONTEXT);
	}

	return globalContext;
}


VError VJSContextPool::_ReleaseContext( VJSGlobalContext* inContext)
{
	VError err = VE_OK;

	if (inContext != NULL)
	{
		if (inContext != NULL)
		{
			if (fDelegate != NULL)
			{
				err = fDelegate->UninitializeJSContext( inContext);
			}

			QuickReleaseRefCountable( inContext);
		}
	}
	return err;
}


bool VJSContextPool::_IsPooled(  XBOX::VJSGlobalContext* inContext) const
{
	if (fUsedContexts.find( inContext) != fUsedContexts.end())
		return true;

	if (fUnusedContexts.find( inContext) != fUnusedContexts.end())
		return true;

	return false;
}


void VJSContextPool::_InitGlobalClasses()
{
	static bool sDone = false;

	if (!sDone)
	{
		sDone = true;

		// Append some custom properties to the global class

		VJSGlobalClass::AddStaticFunction(	kSSJS_PROPERTY_NAME_AddHttpRequestHandler, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_addHttpRequestHandler>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_RemoveHttpRequestHandler, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_removeHttpRequestHandler>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_GetFolder, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_getFolder>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_GetSettingFile, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_getSettingFile>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_GetWalibFolder, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_getWalibFolder>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_GetItemsWithRole, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_getItemsWithRole>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);
	
		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_verifyDataStore, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_verifyDataStore>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_repairDataStore, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_repairInto>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_compactDataStore, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_compactInto>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_loginByKey, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_login>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_loginByPassword, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_unsecureLogin>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_currentUser, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_getCurrentUser>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_currentSession, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_getConnectionSession>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticFunction( kSSJS_PROPERTY_NAME_logout, VJSGlobalClass::js_callStaticFunction<VJSApplicationGlobalObject::_logout>, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);
		
		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Solution, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getSolution>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Name, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getName>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Administrator, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getIsAdministrator>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_HTTPServer, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getHttpServer>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_WebAppService, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getWebAppService>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_DataService, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getDataService>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_RPCService, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getRPCService>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Console, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getConsole>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Pattern, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getPattern>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Storage, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getStorage>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Settings, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getSettings>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Directory, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getDirectory>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Internal, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getInternal>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontEnum | JS4D::PropertyAttributeDontDelete );

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_Permissions, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getPermissions>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_RPCCatalog, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getRPCCatalog>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);

		VJSGlobalClass::AddStaticValue( kSSJS_PROPERTY_NAME_wildchar, VJSGlobalClass::js_getProperty<VJSApplicationGlobalObject::_getWildChar>, NULL, JS4D::PropertyAttributeReadOnly | JS4D::PropertyAttributeDontDelete);


		VJSGlobalClass::CreateGlobalClasses();
		VJSHTTPRequestHeader::Class();
		VJSHTTPResponseHeader::Class();
		VJSHTTPRequest::Class();
		VJSHTTPResponse::Class();
		VJSHTMLForm::Class();
		VJSHTMLFormPart::Class();
		VJSSolution::Class();
		VJSApplication::Class();
		VJSHTTPServer::Class();
		VJSWebAppService::Class();
		VJSDataService::Class();
		VRIAServerJSCore::Class();
		VJSRPCServiceCore::Class();
		VRIAServerJSCore::Class();
	}
}


bool VJSContextPool::_SetSpecific( const VJSContext& inContext, VJSContextPoolSpecific* inSpecific)
{
	return inContext.GetGlobalObjectPrivateInstance()->SetSpecific( 'jspo', inSpecific, VJSSpecifics::DestructorVObject);
}


VJSContextPoolSpecific* VJSContextPool::_GetSpecific( const VJSContext& inContext)
{
	return static_cast<VJSContextPoolSpecific*>( inContext.GetGlobalObjectPrivateInstance()->GetSpecific( 'jspo'));
}



// ----------------------------------------------------------------------------



VRIAJSCallbackGlobalFunction::VRIAJSCallbackGlobalFunction( const VString& inFunctionName)
: fFunctionName(inFunctionName)
{
}


VRIAJSCallbackGlobalFunction::~VRIAJSCallbackGlobalFunction()
{
}


void VRIAJSCallbackGlobalFunction::GetFunctionName( VString& outName)
{
	outName = fFunctionName;
}

			
VError VRIAJSCallbackGlobalFunction::Call( VJSContext& inContext, const std::vector<VJSValue> *inParameters, VJSValue* outResult)
{
	VError err = VE_OK;

	JS4D::ExceptionRef exception = NULL;
	VJSObject object( inContext.GetGlobalObject());

	if (!object.CallMemberFunction( fFunctionName, inParameters, outResult, &exception))
	{
		if (JS4D::ThrowVErrorForException( inContext, exception))
		{
			err = VE_RIA_JS_CALL_TO_FUNCTION_FAILED;
		}
		else
		{
			err = ThrowError( VE_RIA_JS_CALL_TO_FUNCTION_FAILED, &fFunctionName, NULL);
		}
	}

	return err;
}


bool VRIAJSCallbackGlobalFunction::Match( const IRIAJSCallback* inCallback) const
{
	bool result = false;

	const VRIAJSCallbackGlobalFunction *callback = dynamic_cast<const VRIAJSCallbackGlobalFunction*>(inCallback);
	if (callback != NULL)
	{
		result = (fFunctionName.EqualTo( callback->fFunctionName, 1) == 1);
	}

	return result;
}



// ----------------------------------------------------------------------------



VRIAJSCallbackModuleFunction::VRIAJSCallbackModuleFunction( const VString& inModuleName, const VString& inFunctionName)
: fModuleName(inModuleName), fFunctionName(inFunctionName)
{
}


VRIAJSCallbackModuleFunction::~VRIAJSCallbackModuleFunction()
{
}


void VRIAJSCallbackModuleFunction::GetModuleName( VString& outName) const
{
	outName = fModuleName;
}


void VRIAJSCallbackModuleFunction::GetFunctionName( VString& outName) const
{
	outName = fFunctionName;
}


VError VRIAJSCallbackModuleFunction::Call( VJSContext& inContext, const std::vector<VJSValue> *inParameters, VJSValue* outResult)
{
	VError err = VE_OK;

	JS4D::ExceptionRef exception = NULL;
	VJSObject object( inContext.GetGlobalObject());

	VJSValue nameParam( inContext);
	nameParam.SetString( fModuleName);
	std::vector<VJSValue> params;
	params.push_back( nameParam);

	VJSValue module( inContext);
	if (object.CallMemberFunction( L"require", &params, &module, &exception))
	{
		if (!module.IsUndefined() && !module.IsNull())
		{
			VJSObject moduleObject( inContext);
			module.GetObject( moduleObject);
			if (!moduleObject.CallMemberFunction( fFunctionName, inParameters, outResult, &exception))
			{
				JS4D::ThrowVErrorForException( inContext, exception);
				err = VE_RIA_JS_CALL_TO_FUNCTION_FAILED;
			}
		}
		else
		{
			err = VE_RIA_JS_CALL_TO_REQUIRE_FAILED;
		}
	}
	else
	{
		JS4D::ThrowVErrorForException( inContext, exception);
		err = VE_RIA_JS_CALL_TO_REQUIRE_FAILED;
	}

	return err;
}


bool VRIAJSCallbackModuleFunction::Match( const IRIAJSCallback* inCallback) const
{
	bool result = false;

	const VRIAJSCallbackModuleFunction *callback = dynamic_cast<const VRIAJSCallbackModuleFunction*>(inCallback);
	if (callback != NULL)
	{
		if (fModuleName.EqualTo( callback->fModuleName, 0) == 1)
			result = (fFunctionName.EqualTo( callback->fFunctionName, 1) == 1);
	}

	return result;
}

