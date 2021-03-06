#pragma once

typedef struct _AppDomain AppDomain;
typedef struct _InstructionPointerMappingNode InstructionPointerMappingNode;
typedef struct _DomainSpecificMethod DomainSpecificMethod;
typedef struct _StaticGenericField StaticGenericField;

#include <uthash.h>
#include <CLR/GC.h>
#include <CLR/IROpcode.h>
#include <CLR/IRStructures.h>
#include <CLR/MetadataStructures.h>
#include <System/Process.h>

struct _AppDomain
{
	uint32_t DomainIndex;

	Process* Process;
	uint32_t ThreadCount;
	Thread** Threads;
	uint32_t IRAssemblyCount;
	IRAssembly** IRAssemblies;

	TypeDefinition* CachedType___System_Array;
	TypeDefinition* CachedType___System_Boolean;
	TypeDefinition* CachedType___System_Byte;
	TypeDefinition* CachedType___System_Char;
	TypeDefinition* CachedType___System_Double;
	TypeDefinition* CachedType___System_Enum;
	TypeDefinition* CachedType___System_Exception;
	TypeDefinition* CachedType___System_Int16;
	TypeDefinition* CachedType___System_Int32;
	TypeDefinition* CachedType___System_Int64;
	TypeDefinition* CachedType___System_IntPtr;
	TypeDefinition* CachedType___System_Object;
	TypeDefinition* CachedType___System_RuntimeFieldHandle;
	TypeDefinition* CachedType___System_RuntimeMethodHandle;
	TypeDefinition* CachedType___System_RuntimeTypeHandle;
	TypeDefinition* CachedType___System_SByte;
	TypeDefinition* CachedType___System_Single;
	TypeDefinition* CachedType___System_String;
	TypeDefinition* CachedType___System_Type;
	TypeDefinition* CachedType___System_TypedReference;
	TypeDefinition* CachedType___System_UInt16;
	TypeDefinition* CachedType___System_UInt32;
	TypeDefinition* CachedType___System_UInt64;
	TypeDefinition* CachedType___System_UIntPtr;
	TypeDefinition* CachedType___System_ValueType;
	TypeDefinition* CachedType___System_Void;
	TypeDefinition* CachedType___System_Threading_InternalThread;

	void** StaticValues;
	bool_t** StaticConstructorsCalled;

	InstructionPointerMappingNode** InstructionPointerMappingTree;
	DomainSpecificMethod* DomainSpecificMethodsTable;
	StaticGenericField* StaticGenericFieldsTable;

	GC* GarbageCollector;
};

struct _InstructionPointerMappingNode
{
	size_t StartAddress;
	size_t EndAddress;
	IRMethod* Method;
	InstructionPointerMappingNode* Right;
	InstructionPointerMappingNode* Left;
};

struct _DomainSpecificMethod
{
	IRMethod* MethodKey;
	void* AssembledMethod;
	UT_hash_handle HashHandle;
};

struct _StaticGenericField
{
	IRField* Field;
	void* FieldData;
	UT_hash_handle HashHandle;
};

AppDomain* AppDomain_Create(Thread* pMainThread);
void AppDomain_Destroy(AppDomain* pDomain);
void AppDomain_AddThread(AppDomain* pDomain, Thread* pThread);
void AppDomain_RemoveThread(AppDomain* pDomain, Thread* pThread);
void AppDomain_AddAssembly(AppDomain* pDomain, IRAssembly* pAssembly);
void AppDomain_AddInstructionPointerMapping(IRMethod* pMethod, size_t pAddress, size_t pLength);
InstructionPointerMappingNode* AppDomain_FindInstructionPointerMapping(AppDomain* pDomain, size_t pInstructionPointer);
void AppDomain_LinkCorlib(AppDomain* pDomain, CLIFile* pCorlibFile);

ElementType AppDomain_GetElementTypeFromIRType(AppDomain* pDomain, IRType* pType);
IRField* AppDomain_GetIRFieldFromMetadataToken(AppDomain* pDomain, IRAssembly* pAssembly, uint32_t pToken, uint32_t* pFieldIndex);
IRType* AppDomain_GetIRTypeFromElementType(AppDomain* pDomain, ElementType pType);
IRType* AppDomain_GetIRTypeFromSignatureType(AppDomain* pDomain, IRAssembly* pAssembly, SignatureType* pType);
IRType* AppDomain_GetIRTypeFromMetadataToken(AppDomain* pDomain, IRAssembly* pAssembly, uint32_t pToken, bool_t pClassToken);
IRMethod* AppDomain_GetIRMethodFromDefinitionAndSignature(AppDomain* pDomain, IRAssembly* pAssembly, MethodDefinition* pMethodDefinition, SignatureMethodSpecification* pSignatureMethodSpecification);

bool_t AppDomain_IsStructure(AppDomain* pDomain, TypeDefinition* pTypeDefinition);
TypeDefinition* AppDomain_ResolveTypeReference(AppDomain* pDomain, CLIFile* pFile, TypeReference* pTypeReference);
void AppDomain_ResolveMemberReference(AppDomain* pDomain, CLIFile* pFile, MemberReference* pMemberReference);
void AppDomain_ResolveGenericTypeParameters(AppDomain* pDomain, CLIFile* pFile, IRType* pType);
void AppDomain_ResolveGenericMethodParametersFinalize(AppDomain* pDomain, CLIFile* pFile, IRType* pType, IRMethod* pMethod);
void AppDomain_ResolveGenericMethodParameters(AppDomain* pDomain, CLIFile* pFile, IRType* pType, IRMethod* pMethod);

TypeDefinition* AppDomain_GetTypeDefByCanonicalName(AppDomain* pDomain, IRAssembly* pAssembly, char* pCanonicalName);
