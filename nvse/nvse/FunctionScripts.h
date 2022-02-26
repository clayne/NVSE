#pragma once
#include "ScriptUtils.h"
#include <unordered_map>
#include "FastStack.h"

struct UserFunctionParam
{
	UInt16	varIdx;
	UInt8	varType;

	UserFunctionParam(UInt16 _idx, UInt16 _type) : varIdx(_idx), varType(_type) { }
	UserFunctionParam() : varIdx(-1), varType(Script::eVarType_Invalid) { }
};

#if RUNTIME

struct FunctionContext;

// base class for Template Method-ish objects to execute function scripts
// derive from it to allow function scripts to be invoked from script or internal code
class FunctionCaller
{
public:
	virtual ~FunctionCaller() = default;

	virtual UInt8 ReadCallerVersion() = 0;
	virtual Script * ReadScript() = 0;
	virtual bool PopulateArgs(ScriptEventList* eventList, FunctionInfo* info) = 0;

	virtual TESObjectREFR* ThisObj() = 0;
	virtual TESObjectREFR* ContainingObj() = 0;
	virtual Script* GetInvokingScript() { return NULL; }
};

// stores info about function script (params, etc). generated once per function script and cached
struct FunctionInfo
{
	DynamicParamInfo	m_dParamInfo;
	std::vector<UserFunctionParam> m_userFunctionParams;
	Script				* m_script;			// function script
	UInt8				m_functionVersion;	// bytecode version of Function statement
	bool				m_bad;
	UInt8				m_instanceCount;
	ScriptEventList		* m_eventList;		// cached for quicker construction of function script, but requires care when dealing with recursive function calls
#if _DEBUG
	const char*			editorID;
#endif
	UInt8*				m_singleLineLambdaPosition = nullptr;
	bool				m_isLambda;

	FunctionInfo() {}
	FunctionInfo(Script* script);
	~FunctionInfo();

	FunctionContext	* CreateContext(UInt8 version, Script* invokingScript);
	bool IsGood() { return !m_bad; }
	bool IsActive() { return m_instanceCount ? true : false; }
	Script* GetScript() { return m_script; }
	ParamInfo* Params() { return m_dParamInfo.Params(); }
	DynamicParamInfo& ParamInfo() { return m_dParamInfo; }
	UserFunctionParam* GetParam(UInt32 paramIndex);
	bool Execute(FunctionCaller& caller, FunctionContext* context);
	ScriptEventList* GetEventList() { return m_eventList; }
	UInt32 GetParamVarTypes(UInt8* out) const;	// returns count, if > 0 returns types as array
};

// represents a function executing on the stack
struct FunctionContext
{
private:
	FunctionInfo	* m_info;
	ScriptEventList	* m_eventList;		// temporary eventlist generated for function script
	ScriptToken		* m_result;
	Script			* m_invokingScript;
	UInt8			m_callerVersion;
	bool			m_bad;
	bool m_lambdaBackupEventList; // if parent event list can't be retrieved, us
public:
	FunctionContext(FunctionInfo* info, UInt8 version, Script* invokingScript);
	~FunctionContext();

	bool Execute(FunctionCaller & caller);
	bool Return(ExpressionEvaluator* eval);
	bool IsGood() { return !m_bad; }
	ScriptToken*  Result() { return m_result; }
	FunctionInfo* Info() { return m_info; }
	Script* InvokingScript() { return m_invokingScript; }
	void* operator new(size_t size);
	void operator delete(void* p);
};

// controls user function calls.
// Manages a stack of function contexts
// Function args in Call bytecode. FunctionInfo encoded in Begin Function data. Return value from SetFunctionValue.
class UserFunctionManager
{
	static UserFunctionManager	* GetSingleton();

	UserFunctionManager();

	static constexpr UInt32	kMaxNestDepth = 30;	// arbitrarily low; have seen 180+ nested calls execute w/o problems

	UInt32								m_nestDepth;
	Stack<FunctionContext*>		m_functionStack;
	UnorderedMap<Script*, FunctionInfo>	m_functionInfos;

	// these take a ptr to the function script to check that it matches executing script
	FunctionContext* Top(Script* funcScript);
	bool Pop(Script* funcScript);
	void Push(FunctionContext* context) { m_functionStack.Push(context); }
	FunctionInfo* GetFunctionInfo(Script* funcScript);

public:
	~UserFunctionManager();

	enum { kVersion = 1 };	// increment when bytecode representation changes

	static ScriptToken* Call(ExpressionEvaluator* eval);
	static bool	Return(ExpressionEvaluator* eval);
	static bool Enter(Script* funcScript);
	static ScriptToken* Call(FunctionCaller&& caller);
	static UInt32 GetFunctionParamTypes(Script* fnScript, UInt8* typesOut);

	// return script that called fnScript
	static Script* GetInvokingScript(Script* fnScript);

	static void ClearInfos();
};

// allows us to call function scripts directly
class InternalFunctionCaller : public FunctionCaller
{
	template <bool IsSelfOwning>
	bool PopulateArgs_Templ(ScriptEventList* eventList, FunctionInfo* info,
		const ArrayElement_Templ<IsSelfOwning>* altElemArgs);

public:
	InternalFunctionCaller(Script* script, TESObjectREFR* callingObj = NULL, TESObjectREFR* container = NULL)
		: m_callerVersion(UserFunctionManager::kVersion), m_numArgs(0), m_script(script), m_thisObj(callingObj), m_container(container) { }

	~InternalFunctionCaller() override = default;
	UInt8 ReadCallerVersion() override { return m_callerVersion; }
	Script* ReadScript() override { return m_script; }

	bool PopulateArgs(ScriptEventList* eventList, FunctionInfo* info) override;

	TESObjectREFR* ThisObj() override { return m_thisObj; }
	TESObjectREFR* ContainingObj() override { return m_container; }

	bool SetArgs(UInt8 numArgs, ...);
	bool vSetArgs(UInt8 numArgs, va_list args);
	bool SetArgsRaw(UInt8 numArgs, const void* args);

	template <bool IsSelfOwning>
	bool SetArgs(UInt8 numArgs, const ArrayElement_Templ<IsSelfOwning> *elemArgs);

	template <bool IsSelfOwning>
	void GetPopulateArgFunctions(const ArrayElement_Templ<IsSelfOwning>* altElemArgs,
		std::function<void(UInt32, UInt32&)>& formFunc,
		std::function<void(UInt32, double&)>& floatFunc,
		std::function<void(UInt32, double&)>& intFunc,
		std::function<void(UInt32, const char*&)>& strFunc,
		std::function<void(UInt32, ArrayID&)>& arrFunc) const;

protected:

	UInt8			m_callerVersion;
	UInt8			m_numArgs;
	Script			* m_script;
	void			* m_args[kMaxUdfParams];
	
	//Used because for number-type args, the arrElem always contains a double, which can easily be cast to int if needed.
	//In contrast, the SetArgs funcs that fill m_args depend on casting either an int or float value to void*,
	//and we lose track of if it was an int or float.
	//The above causes a bug where the data can be wrongly interpreted if the UDF numeric arg to be populated is
	//of the opposite type (float/int) as what we stored in m_args.
	std::variant<const ArrayElement*, const SelfOwningArrayElement*>
	m_altElemArgs = static_cast<ArrayElement*>(nullptr);

	TESObjectREFR	* m_thisObj;
	TESObjectREFR	* m_container;

	virtual bool ValidateParam(UserFunctionParam* param, UInt8 paramIndex) { return param != nullptr; }
};

namespace PluginAPI {
	bool CallFunctionScript(Script* fnScript, TESObjectREFR* callingObj, TESObjectREFR* container,
		NVSEArrayVarInterface::Element* result, UInt8 numArgs, ...);
	bool CallFunctionScriptAlt(Script *fnScript, TESObjectREFR *callingObj, UInt8 numArgs, ...);
}


///// InternalFunctionCaller ///////
/// (since template funcs must be defined in header) ///

template <bool IsSelfOwning>
bool InternalFunctionCaller::SetArgs(UInt8 numArgs, const ArrayElement_Templ<IsSelfOwning>* elemArgs)
{
	if (numArgs > kMaxUdfParams)
		return false;

	m_numArgs = numArgs;
	m_altElemArgs = elemArgs;

	return true;
}

template <bool IsSelfOwning>
void InternalFunctionCaller::GetPopulateArgFunctions(const ArrayElement_Templ<IsSelfOwning>* altElemArgs,
	std::function<void(UInt32, UInt32&)> &formFunc,
	std::function<void(UInt32, double&)> &floatFunc,
	std::function<void(UInt32, double&)> &intFunc,
	std::function<void(UInt32, const char*&)> &strFunc,
	std::function<void(UInt32, ArrayID&)> &arrFunc) const
{

	constexpr char altElemExtractionErrorMsg[] = "Cached argument #%u for function script is not %s type, yet the UDF expects that type.";

	// Define lambdas that will be used to define the funcs.

	auto const populateFormArg = [this](UInt32 i, UInt32& formId_out)
	{
		if (auto const form = static_cast<TESForm*>(m_args[i]))
		{
			formId_out = form->refID;
		}
	};
	auto const populateFormArg_Alt = [altElemArgs, this](UInt32 i, UInt32& formId_out)
	{
		if (!altElemArgs[i].GetAsFormID(&formId_out))
		{
			ShowRuntimeError(m_script, altElemExtractionErrorMsg, i, "Form");
		}
	};

	auto const populateFloatArg = [this](UInt32 i, double& varData_out)
	{
		varData_out = *((float*)&m_args[i]);
	};
	auto const populateFloatArg_Alt = [altElemArgs, this](UInt32 i, double& varData_out)
	{
		if (!altElemArgs[i].GetAsNumber(&varData_out))
		{
			ShowRuntimeError(m_script, altElemExtractionErrorMsg, i, "Number");
		}
	};

	auto const populateIntArg = [this](UInt32 i, double& varData_out)
	{
		varData_out = reinterpret_cast<SInt32>(m_args[i]);
	};
	auto const populateIntArg_Alt = [&](UInt32 i, double& varData_out)
	{
		populateFloatArg_Alt(i, varData_out);
		varData_out = static_cast<SInt32>(varData_out);	//floor
	};

	auto const populateStrArg = [this](UInt32 i, const char*& str_out)
	{
		str_out = static_cast<const char*>(m_args[i]);
	};
	auto const populateStrArg_Alt = [altElemArgs, this](UInt32 i, const char*& str_out)
	{
		if (!altElemArgs[i].GetAsString(&str_out))
		{
			ShowRuntimeError(m_script, altElemExtractionErrorMsg, i, "String");
		}
	};

	auto const populateArrArg = [this](UInt32 i, ArrayID& arr_out)
	{
		arr_out = reinterpret_cast<ArrayID>(m_args[i]);
	};
	auto const populateArrArg_Alt = [altElemArgs, this](UInt32 i, ArrayID& arr_out)
	{
		if (!altElemArgs[i].GetAsArray(&arr_out))
		{
			ShowRuntimeError(m_script, altElemExtractionErrorMsg, i, "Array");
		}
	};

	// Define the passed funcs with our lambdas.
	if (altElemArgs)
	{
		formFunc = populateFormArg_Alt;
		floatFunc = populateFloatArg_Alt;
		intFunc = populateIntArg_Alt;
		strFunc = populateStrArg_Alt;
		arrFunc = populateArrArg_Alt;
	}
	else
	{
		formFunc = populateFormArg;
		floatFunc = populateFloatArg;
		intFunc = populateIntArg;
		strFunc = populateStrArg;
		arrFunc = populateArrArg;
	}
}

template <bool IsSelfOwning>
bool InternalFunctionCaller::PopulateArgs_Templ(ScriptEventList* eventList, FunctionInfo* info,
	const ArrayElement_Templ<IsSelfOwning>* altElemArgs)
{
	DynamicParamInfo& dParams = info->ParamInfo();
	if (dParams.NumParams() > kMaxUdfParams)
	{
		return false;
	}

	std::function<void(UInt32, UInt32&)> populateFormArgFunc;
	std::function<void(UInt32, double&)> populateFloatArgFunc;
	std::function<void(UInt32, double&)> populateIntArgFunc;
	std::function<void(UInt32, const char*&)> populateStrArgFunc;
	std::function<void(UInt32, ArrayID&)> populateArrArgFunc;
	GetPopulateArgFunctions(altElemArgs, populateFormArgFunc, populateFloatArgFunc, populateIntArgFunc, populateStrArgFunc, populateArrArgFunc);

	// populate the args in the event list
	for (UInt32 i = 0; i < m_numArgs; i++)
	{
		UserFunctionParam* param = info->GetParam(i);
		if (!ValidateParam(param, i))
		{
			ShowRuntimeError(m_script, "Failed to extract parameter %d. Please verify the number of parameters in function script match those required for event.", i);
			return false;
		}

		ScriptLocal* var = eventList->GetVariable(param->varIdx);
		if (!var)
		{
			ShowRuntimeError(m_script, "Could not look up argument variable for function script");
			return false;
		}

		switch (param->varType)
		{
		case Script::eVarType_Integer:
			populateIntArgFunc(i, var->data);
			break;
		case Script::eVarType_Float:
			populateFloatArgFunc(i, var->data);
			break;
		case Script::eVarType_Ref:
		{
			UInt32 formID = 0;
			populateFormArgFunc(i, formID);
			*((UInt32*)&var->data) = formID;
		}
		break;
		case Script::eVarType_String:
		{
			const char* str = nullptr;
			populateStrArgFunc(i, str);
			var->data = g_StringMap.Add(info->GetScript()->GetModIndex(), str, true);
			AddToGarbageCollection(eventList, var, NVSEVarType::kVarType_String);
			break;
		}
		case Script::eVarType_Array:
		{
			ArrayID arrID = 0;
			populateArrArgFunc(i, arrID);
			if (g_ArrayMap.Get(arrID))
			{
				g_ArrayMap.AddReference(&var->data, arrID, info->GetScript()->GetModIndex());
				AddToGarbageCollection(eventList, var, NVSEVarType::kVarType_Array);
			}
			else
				var->data = 0;
		}
		break;
		default:
			// wtf?
			ShowRuntimeError(m_script, "Unexpected param type %02X in internal function call", param->varType);
			return false;
		}
	}

	return true;
}

#endif
