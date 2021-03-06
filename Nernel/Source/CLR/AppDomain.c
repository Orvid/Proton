#include <CLR/AppDomain.h>
#include <CLR/ILDecomposition.h>
#include <CLR/IROptimizer.h>
#include <System/Atomics.h>
#include <System/Multiboot.h>

AppDomain** gDomainRegistry = NULL;
uint32_t gDomainRegistry_NextIndex = 0;
uint8_t gDomainRegistry_Busy = 0;

void AppDomainRegistry_AddDomain(AppDomain* pDomain)
{
	Atomic_AquireLock(&gDomainRegistry_Busy);
	gDomainRegistry = (AppDomain**)realloc(gDomainRegistry, sizeof(AppDomain*) * (gDomainRegistry_NextIndex + 1));
	gDomainRegistry[gDomainRegistry_NextIndex] = pDomain;
	pDomain->DomainIndex = gDomainRegistry_NextIndex;
	gDomainRegistry_NextIndex++;
	Atomic_ReleaseLock(&gDomainRegistry_Busy);
}



AppDomain* AppDomain_Create(Thread* pMainThread)
{
	LoadedModule* loadedModule = Multiboot_GetLoadedModule("/gac/mscorlib.dll");
	if (loadedModule)
	{
		CLIFile* cliFile = CLIFile_Create((uint8_t*)loadedModule->Address, loadedModule->Size, "mscorlib.dll");
		if (cliFile)
		{
			AppDomain* domain = (AppDomain*)calloc(1, sizeof(AppDomain));
			domain->Process = pMainThread->Process;
			domain->ThreadCount = 8;
			domain->Threads = (Thread**)calloc(1, domain->ThreadCount * sizeof(Thread*));
			domain->Threads[0] = pMainThread;
			pMainThread->Domain = domain;
			Process_AddDomain(domain->Process, domain);
			GC_Create(domain);
			Log_WriteLine(LOGLEVEL__Memory, "Memory: AppDomain_Create @ 0x%x", (unsigned int)domain);
			AppDomainRegistry_AddDomain(domain);
			AppDomain_LinkCorlib(domain, cliFile);
			ILDecomposition_CreateAssembly(domain, cliFile);
			return domain;
		}
		else
		{
			Panic("An error occured while loading corlib!");
		}
	}
	else
	{
		Panic("An error occured while loading corlib module!");
	}
}

void AppDomain_Destroy(AppDomain* pDomain)
{
	if (pDomain->InstructionPointerMappingTree) free(pDomain->InstructionPointerMappingTree);
	for (uint32_t index = 0; index < pDomain->ThreadCount; ++index)
	{
		if (pDomain->Threads[index])
		{
			Thread_Destroy(pDomain->Threads[index]);
		}
	}
	free(pDomain->Threads);
	for (uint32_t index = 0; index < pDomain->IRAssemblyCount; ++index)
	{
		if (pDomain->IRAssemblies[index])
		{
			IRAssembly_Destroy(pDomain->IRAssemblies[index]);
		}
	}
	free(pDomain->IRAssemblies);
	if (pDomain->StaticValues)
	{
		for (uint32_t index = 0; index < pDomain->IRAssemblyCount; ++index)
		{
			if (pDomain->StaticValues[index])
			{
				free(pDomain->StaticValues[index]);
			}
		}
		free(pDomain->StaticValues);
	}
	Process_RemoveDomain(pDomain->Process, pDomain);
	GC_Destroy(pDomain->GarbageCollector);
	free(pDomain);
}

void AppDomain_AddThread(AppDomain* pDomain, Thread* pThread)
{
	bool_t foundUnused = FALSE;
	uint32_t unusedIndex = 0;
	for (uint32_t index = 0; index < pDomain->ThreadCount; ++index)
	{
		if (!pDomain->Threads[index])
		{
			unusedIndex = index;
			foundUnused = TRUE;
			break;
		}
	}
	if (!foundUnused)
	{
		unusedIndex = pDomain->ThreadCount;
		pDomain->ThreadCount <<= 1;
		pDomain->Threads = (Thread**)realloc(pDomain->Threads, pDomain->ThreadCount * sizeof(Thread*));
	}
	pThread->Domain = pDomain;
	pDomain->Threads[unusedIndex] = pThread;
}

void AppDomain_RemoveThread(AppDomain* pDomain, Thread* pThread)
{
	for (uint32_t index = 0; index < pDomain->ThreadCount; ++index)
	{
		if (pDomain->Threads[index] == pThread)
		{
			pDomain->Threads[index] = NULL;
			break;
		}
	}
}

void AppDomain_AddAssembly(AppDomain* pDomain, IRAssembly* pAssembly)
{
	pAssembly->AssemblyIndex = pDomain->IRAssemblyCount;
	pDomain->IRAssemblyCount++;
	pDomain->IRAssemblies = (IRAssembly**)realloc(pDomain->IRAssemblies, sizeof(IRAssembly*) * pDomain->IRAssemblyCount);
	pDomain->IRAssemblies[pAssembly->AssemblyIndex] = pAssembly;
	pDomain->StaticValues = (void**)realloc(pDomain->StaticValues, pDomain->IRAssemblyCount * sizeof(void*));
	pDomain->StaticConstructorsCalled = (bool_t**)realloc(pDomain->StaticConstructorsCalled, pDomain->IRAssemblyCount * sizeof(bool_t*));
	pDomain->InstructionPointerMappingTree = (InstructionPointerMappingNode**)realloc(pDomain->InstructionPointerMappingTree, pDomain->IRAssemblyCount * sizeof(InstructionPointerMappingNode*));
	pDomain->InstructionPointerMappingTree[pAssembly->AssemblyIndex] = NULL;
}

void AppDomain_InsertInstructionPointerMapping(InstructionPointerMappingNode* pCurrentNode, InstructionPointerMappingNode* pNewNode)
{
	if (pCurrentNode->StartAddress == pNewNode->StartAddress) Panic("InsertInstructionPointerMapping Duplicate Address");
	if (pNewNode->StartAddress < pCurrentNode->StartAddress)
	{
		if (pCurrentNode->Left) AppDomain_InsertInstructionPointerMapping(pCurrentNode->Left, pNewNode);
		else pCurrentNode->Left = pNewNode;
	}
	else
	{
		if (pCurrentNode->Right) AppDomain_InsertInstructionPointerMapping(pCurrentNode->Right, pNewNode);
		else pCurrentNode->Right = pNewNode;
	}
}

void AppDomain_AddInstructionPointerMapping(IRMethod* pMethod, size_t pAddress, size_t pLength)
{
	InstructionPointerMappingNode* node = (InstructionPointerMappingNode*)calloc(1, sizeof(InstructionPointerMappingNode));
	node->StartAddress = pAddress;
	node->EndAddress = pAddress + pLength;
	node->Method = pMethod;

	InstructionPointerMappingNode* rootNode = pMethod->ParentAssembly->ParentDomain->InstructionPointerMappingTree[pMethod->ParentAssembly->AssemblyIndex];
	if (!rootNode) pMethod->ParentAssembly->ParentDomain->InstructionPointerMappingTree[pMethod->ParentAssembly->AssemblyIndex] = node;
	else AppDomain_InsertInstructionPointerMapping(rootNode, node);
}

InstructionPointerMappingNode* AppDomain_SearchInstructionPointerMapping(InstructionPointerMappingNode* pCurrentNode, size_t pInstructionPointer)
{
	if (!pCurrentNode) return NULL;
	if (pCurrentNode->StartAddress <= pInstructionPointer && pCurrentNode->EndAddress > pInstructionPointer) return pCurrentNode;
	if (pCurrentNode->StartAddress > pInstructionPointer) return AppDomain_SearchInstructionPointerMapping(pCurrentNode->Left, pInstructionPointer);
	return AppDomain_SearchInstructionPointerMapping(pCurrentNode->Right, pInstructionPointer);
}

InstructionPointerMappingNode* AppDomain_FindInstructionPointerMapping(AppDomain* pDomain, size_t pInstructionPointer)
{
	InstructionPointerMappingNode* node = NULL;
	for (uint32_t index = 0; index < pDomain->IRAssemblyCount; ++index)
	{
		node = AppDomain_SearchInstructionPointerMapping(pDomain->InstructionPointerMappingTree[index], pInstructionPointer);
		if (node) break;
	}
	return node;
}

void AppDomain_LinkCorlib(AppDomain* pDomain, CLIFile* pCorlibFile)
{
	for (uint32_t i = 1; i <= pCorlibFile->TypeDefinitionCount; i++)
	{
		TypeDefinition* type = &(pCorlibFile->TypeDefinitions[i]);
		if (!strcmp(type->Namespace, "System"))
		{
			if (!strcmp(type->Name, "Array"))
			{
				pDomain->CachedType___System_Array = type;
			}
			else if (!strcmp(type->Name, "Boolean"))
			{
				pDomain->CachedType___System_Boolean = type;
			}
			else if (!strcmp(type->Name, "Byte"))
			{
				pDomain->CachedType___System_Byte = type;
			}
			else if (!strcmp(type->Name, "Char"))
			{
				pDomain->CachedType___System_Char = type;
			}
			else if (!strcmp(type->Name, "Double"))
			{
				pDomain->CachedType___System_Double = type;
			}
			else if (!strcmp(type->Name, "Enum"))
			{
				pDomain->CachedType___System_Enum = type;
			}
			else if (!strcmp(type->Name, "Exception"))
			{
				pDomain->CachedType___System_Exception = type;
			}
			else if (!strcmp(type->Name, "Int16"))
			{
				pDomain->CachedType___System_Int16 = type;
			}
			else if (!strcmp(type->Name, "Int32"))
			{
				pDomain->CachedType___System_Int32 = type;
			}
			else if (!strcmp(type->Name, "Int64"))
			{
				pDomain->CachedType___System_Int64 = type;
			}
			else if (!strcmp(type->Name, "IntPtr"))
			{
				pDomain->CachedType___System_IntPtr = type;
			}
			else if (!strcmp(type->Name, "Object"))
			{
				pDomain->CachedType___System_Object = type;
			}
			else if (!strcmp(type->Name, "RuntimeFieldHandle"))
			{
				pDomain->CachedType___System_RuntimeFieldHandle = type;
			}
			else if (!strcmp(type->Name, "RuntimeMethodHandle"))
			{
				pDomain->CachedType___System_RuntimeMethodHandle = type;
			}
			else if (!strcmp(type->Name, "RuntimeTypeHandle"))
			{
				pDomain->CachedType___System_RuntimeTypeHandle = type;
			}
			else if (!strcmp(type->Name, "SByte"))
			{
				pDomain->CachedType___System_SByte = type;
			}
			else if (!strcmp(type->Name, "Single"))
			{
				pDomain->CachedType___System_Single = type;
			}
			else if (!strcmp(type->Name, "String"))
			{
				pDomain->CachedType___System_String = type;
			}
			else if (!strcmp(type->Name, "Type"))
			{
				pDomain->CachedType___System_Type = type;
			}
			else if (!strcmp(type->Name, "TypedReference"))
			{
				pDomain->CachedType___System_TypedReference = type;
			}
			else if (!strcmp(type->Name, "UInt16"))
			{
				pDomain->CachedType___System_UInt16 = type;
			}
			else if (!strcmp(type->Name, "UInt32"))
			{
				pDomain->CachedType___System_UInt32 = type;
			}
			else if (!strcmp(type->Name, "UInt64"))
			{
				pDomain->CachedType___System_UInt64 = type;
			}
			else if (!strcmp(type->Name, "UIntPtr"))
			{
				pDomain->CachedType___System_UIntPtr = type;
			}
			else if (!strcmp(type->Name, "ValueType"))
			{
				pDomain->CachedType___System_ValueType = type;
			}
			else if (!strcmp(type->Name, "Void"))
			{
				pDomain->CachedType___System_Void = type;
			}
		}
		else if (!strcmp(type->Namespace, "System.Threading"))
		{
			if (!strcmp(type->Name, "InternalThread"))
			{
				pDomain->CachedType___System_Threading_InternalThread = type;
			}
		}
	}
}

IRType* AppDomain_GetIRTypeFromSignatureType(AppDomain* pDomain, IRAssembly* pAssembly, SignatureType* pType)
{
	IRType* type = NULL;
	switch(pType->ElementType)
	{
		case SignatureElementType_I1:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_SByte->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_SByte);
			}
			break;
		case SignatureElementType_U1:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Byte->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Byte);
			}
			break;
		case SignatureElementType_I2:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Int16->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Int16);
			}
			break;
		case SignatureElementType_U2:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UInt16->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_UInt16);
			}
			break;
		case SignatureElementType_I4:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Int32->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Int32);
			}
			break;
		case SignatureElementType_U4:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UInt32->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_UInt32);
			}
			break;
		case SignatureElementType_I8:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Int64->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Int64);
			}
			break;
		case SignatureElementType_U8:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UInt64->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_UInt64);
			}
			break;
		case SignatureElementType_R4:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Single->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Single);
			}
			break;
		case SignatureElementType_R8:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Double->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Double);
			}
			break;
		case SignatureElementType_Boolean:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Boolean->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Boolean);
			}
			break;
		case SignatureElementType_Char:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Char->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Char);
			}
			break;
		case SignatureElementType_IPointer:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_IntPtr->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_IntPtr);
			}
			break;
		case SignatureElementType_UPointer:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UIntPtr->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_UIntPtr);
			}
			break;
		case SignatureElementType_Object:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Object->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Object);
			}
			break;
		case SignatureElementType_String:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_String->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_String);
			}
			break;
		case SignatureElementType_Type:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Type->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Type);
			}
			break;
		case SignatureElementType_Void:
			if (!(type = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Void->TableIndex - 1]))
			{
				type = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Void);
			}
			break;
		case SignatureElementType_ValueType:
		case SignatureElementType_Class:
		{
			MetadataToken* token = NULL;
			if (pType->ElementType == SignatureElementType_ValueType) token = CLIFile_ExpandTypeDefRefOrSpecToken(pAssembly->ParentFile, pType->ValueTypeDefOrRefOrSpecToken);
			else token = CLIFile_ExpandTypeDefRefOrSpecToken(pAssembly->ParentFile, pType->ClassTypeDefOrRefOrSpecToken);
			switch(token->Table)
			{
				case MetadataTable_TypeDefinition:
					if (!(type = pAssembly->Types[((TypeDefinition*)token->Data)->TableIndex - 1]))
					{
						type = IRType_Create(pAssembly, (TypeDefinition*)token->Data);
					}
					break;

				case MetadataTable_TypeReference:
					if (!((TypeReference*)token->Data)->ResolvedType)
					{
						((TypeReference*)token->Data)->ResolvedType = AppDomain_ResolveTypeReference(pDomain, pAssembly->ParentFile, (TypeReference*)token->Data);
					}
					if (!(type = ((TypeReference*)token->Data)->ResolvedType->File->Assembly->Types[((TypeReference*)token->Data)->ResolvedType->TableIndex - 1]))
					{
						type = IRType_Create(((TypeReference*)token->Data)->ResolvedType->File->Assembly, ((TypeReference*)token->Data)->ResolvedType);
					}
					break;

				default:
					Panic("AppDomain_GetIRTypeFromSignatureType Invalid Class Table");
					break;
			}
			CLIFile_DestroyMetadataToken(token);
			break;
		}
		case SignatureElementType_Array:
		{
			IRType* sysArrayType = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Array->TableIndex - 1];
			IRType* elementType = AppDomain_GetIRTypeFromSignatureType(pDomain, pAssembly, pType->ArrayType);
			IRArrayType* lookupType = NULL;
			HASH_FIND(HashHandle, pAssembly->ArrayTypesHashTable, (void*)&elementType, sizeof(void*), lookupType);
			if (!lookupType)
			{
				type = IRType_Copy(sysArrayType);
				type->IsArrayType = TRUE;
				type->ArrayType = IRArrayType_Create(type, elementType);
				HASH_ADD(HashHandle, pAssembly->ArrayTypesHashTable, ElementType, sizeof(void*), type->ArrayType);
			}
			else type = lookupType->ArrayType;
			break;
		}
		case SignatureElementType_SingleDimensionArray:
		{
			IRType* sysArrayType = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Array->TableIndex - 1];
			IRType* elementType = AppDomain_GetIRTypeFromSignatureType(pDomain, pAssembly, pType->SZArrayType);
			IRArrayType* lookupType = NULL;
			HASH_FIND(HashHandle, pAssembly->ArrayTypesHashTable, (void*)&elementType, sizeof(void*), lookupType);
			if (!lookupType)
			{
				type = IRType_Copy(sysArrayType);
				type->IsArrayType = TRUE;
				type->ArrayType = IRArrayType_Create(type, elementType);
				if (elementType->IsGenericParameter)
				{
					//printf("This is why.\n");
				}
				//printf("Couldn't find an array type already created so creating one ElementType: %s.%s @ 0x%x ArrayType at 0x%x\n", elementType->TypeDefinition->Namespace, elementType->TypeDefinition->Name, (unsigned int)elementType, (unsigned int)type);
				HASH_ADD(HashHandle, pAssembly->ArrayTypesHashTable, ElementType, sizeof(void*), type->ArrayType);
			}
			else 
			{
				type = lookupType->ArrayType;
				//printf("Found an array type already created so using one with the ElementType: %s.%s @ 0x%x ArrayType at 0x%x\n", lookupType->ElementType->TypeDefinition->Namespace, lookupType->ElementType->TypeDefinition->Name, (unsigned int)lookupType->ElementType, (unsigned int)lookupType->ArrayType);
			}
			break;
		}
		case SignatureElementType_Pointer:
		{
			IRType* typePointedTo = NULL;
			if (pType->PtrVoid)
			{
				if (!(typePointedTo = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Void->TableIndex - 1]))
				{
					typePointedTo = IRType_Create(pDomain->IRAssemblies[0], pDomain->CachedType___System_Void);
				}
			}
			else
			{		
				typePointedTo = AppDomain_GetIRTypeFromSignatureType(pDomain, pAssembly, pType->PtrType);
			}
			IRPointerType* lookupType = NULL;
			HASH_FIND(HashHandle, pAssembly->PointerTypesHashTable, (void*)&typePointedTo, sizeof(void*), lookupType);
			if (!lookupType)
			{
				IRType* sysPointerType = pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_IntPtr->TableIndex - 1];
				type = IRType_Copy(sysPointerType);
				type->IsPointerType = TRUE;
				type->PointerType = IRPointerType_Create(type, typePointedTo);
				HASH_ADD(HashHandle, pAssembly->PointerTypesHashTable, TypePointedTo, sizeof(void*), type->PointerType);
			}
			else
			{
				type = lookupType->PointerType;
			}
			break;
		}
		case SignatureElementType_MethodVar:
		{
			type = IRAssembly_CreateGenericParameter(pAssembly, FALSE, pType->MVarNumber);
			break;
		}
		case SignatureElementType_Var:
		{
			type = IRAssembly_CreateGenericParameter(pAssembly, TRUE, pType->VarNumber);
			break;
		}
		case SignatureElementType_GenericInstantiation:
		{
			MetadataToken* token = CLIFile_ExpandTypeDefRefOrSpecToken(pAssembly->ParentFile, pType->GenericInstTypeDefOrRefOrSpecToken);
			switch(token->Table)
			{
				case MetadataTable_TypeDefinition:
					if (!(type = pAssembly->Types[((TypeDefinition*)token->Data)->TableIndex - 1]))
					{
						type = IRType_Create(pAssembly, (TypeDefinition*)token->Data);
					}
					break;

				case MetadataTable_TypeReference:
					if (!((TypeReference*)token->Data)->ResolvedType)
					{
						((TypeReference*)token->Data)->ResolvedType = AppDomain_ResolveTypeReference(pDomain, pAssembly->ParentFile, (TypeReference*)token->Data);
					}
					if (!(type = ((TypeReference*)token->Data)->ResolvedType->File->Assembly->Types[((TypeReference*)token->Data)->ResolvedType->TableIndex - 1]))
					{
						type = IRType_Create(((TypeReference*)token->Data)->ResolvedType->File->Assembly, ((TypeReference*)token->Data)->ResolvedType);
					}
					break;

				default:
					Panic("AppDomain_GetIRTypeFromSignatureType Invalid GenericInstantiation Table");
					break;
			}
			bool_t isInstantiated = TRUE;
			for(uint32_t i = 0; i < pType->GenericInstGenericArgumentCount; i++)
			{
				if (pType->GenericInstGenericArguments[i]->ElementType == SignatureElementType_Var)
				{
					isInstantiated = FALSE;
					break;
				}
			}
			if (!isInstantiated)
			{
				CLIFile_DestroyMetadataToken(token);
				break;
			}
			//printf("Yep, it's coming from here\n");
			IRGenericType key;
			memset(&key, 0, sizeof(IRGenericType));
			key.GenericType = type;
			if (type->GenericParameterCount != pType->GenericInstGenericArgumentCount)
			{
				//printf("type->TypeDef->GenericParameterCount (%u), type->GenericParameterCount (%u) != pType->GenericInstGenericArgumentCount (%u)\n", (unsigned int)type->TypeDefinition->GenericParameterCount, (unsigned int)type->GenericParameterCount, (unsigned int)pType->GenericInstGenericArgumentCount);
				//Panic("AppDomain_GetIRTypeFromSignatureType GenericInstantiation Parameter Count Mismatch");
			}
			key.ParameterCount = pType->GenericInstGenericArgumentCount;
			bool_t returnGenericParent = FALSE;
			for (uint32_t index = 0; index < pType->GenericInstGenericArgumentCount; ++index)
			{
				key.Parameters[index] = AppDomain_GetIRTypeFromSignatureType(pDomain, pAssembly, pType->GenericInstGenericArguments[index]);
				if (key.Parameters[index]->IsGenericParameter)
				{
					returnGenericParent = TRUE;
					break;
				}
			}
			//printf("Yep, it's coming from here %i\n", (int)pAssembly->AssemblyIndex);
			if (!returnGenericParent)
			{
				IRType* implementationType = NULL;
				IRGenericType* lookupType = NULL;
				HASH_FIND(HashHandle, pAssembly->GenericTypesHashTable, &key, offsetof(IRGenericType, ImplementationType), lookupType);
				if (!lookupType)
				{
					implementationType = IRType_GenericDeepCopy(type, pAssembly);
					implementationType->GenericType = IRGenericType_Create(type, implementationType);
					implementationType->GenericType->ParameterCount = pType->GenericInstGenericArgumentCount;
					for (uint32_t index = 0; index < pType->GenericInstGenericArgumentCount; ++index)
					{
						implementationType->GenericType->Parameters[index] = key.Parameters[index];
						//printf("GenericInstantiation Type %s.%s, Param %u Type %s.%s\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index, implementationType->GenericType->Parameters[index]->TypeDefinition->Namespace, implementationType->GenericType->Parameters[index]->TypeDefinition->Name);
					}
					HASH_ADD(HashHandle, pAssembly->GenericTypesHashTable, GenericType, offsetof(IRGenericType, ImplementationType), implementationType->GenericType);
					IRType_GenericDeepCopy_Finalize(type, pAssembly, implementationType);
					AppDomain_ResolveGenericTypeParameters(pDomain, pAssembly->ParentFile, implementationType);
					implementationType->IsGenericInstantiation = TRUE;
				}
				else
				{
					implementationType = lookupType->ImplementationType;
				}
				type = implementationType;
			}
			CLIFile_DestroyMetadataToken(token);
			break;
		}
		default:
			printf("Unknown Signature Element Type = 0x%x\n", (unsigned int)pType->ElementType);
			Panic("AppDomain_GetIRTypeFromSignatureType Unknown Signature Element Type");
			break;
	}
	if (!type) Panic("AppDomain_GetIRTypeFromSignatureType returning NULL!");
	return type;
}

IRField* AppDomain_GetIRFieldFromMetadataToken(AppDomain* pDomain, IRAssembly* pAssembly, uint32_t pToken, uint32_t* pFieldIndex)
{
	MetadataToken* token = CLIFile_ExpandMetadataToken(pAssembly->ParentFile, pToken);
	IRField* field = NULL;
	switch(token->Table)
	{
		case MetadataTable_StandAloneSignature:
		{
			Panic("AppDomain_GetIRTypeFromMetadataToken: StandAloneSignatures not yet supported\n");
			break;
		}
		case MetadataTable_Field:
		{
			Field* fieldDef = (Field*)token->Data;
			if (fieldDef->Flags & FieldAttributes_Static)
			{
				field = fieldDef->File->Assembly->Fields[fieldDef->TableIndex - 1];
			}
			else
			{
				IRType* type = fieldDef->TypeDefinition->File->Assembly->Types[fieldDef->TypeDefinition->TableIndex - 1];
				for (uint32_t index = 0; index < type->FieldCount; index++)
				{
					if (!strcmp(fieldDef->Name, type->Fields[index]->FieldDefinition->Name))
					{
						if (Signature_Equals(fieldDef->Signature, fieldDef->SignatureLength, type->Fields[index]->FieldDefinition->Signature, type->Fields[index]->FieldDefinition->SignatureLength))
						{
							field = type->Fields[index];
							*pFieldIndex = index;
							break;
						}
					}
				}
				if (!field)
				{
					Panic("AppDomain_GetIRFieldFromMetadataToken: Unable to resolve Field");
				}
			}
			break;
		}
		case MetadataTable_MemberReference:
		{
			AppDomain_ResolveMemberReference(pDomain, pAssembly->ParentFile, ((MemberReference*)token->Data));
			Field* fieldDef = ((MemberReference*)token->Data)->Resolved.Field;
			if (!fieldDef->SignatureCache) fieldDef->SignatureCache = FieldSignature_Expand(fieldDef->Signature, fieldDef->File);
			
			if (fieldDef->Flags & FieldAttributes_Static)
			{
				if (fieldDef->SignatureCache->Type->ElementType == SignatureElementType_Var)
				{
					Panic("Not currently supported! -W-");
				}
				field = fieldDef->File->Assembly->Fields[fieldDef->TableIndex - 1];
			}
			else
			{
				IRType* type;
				if (fieldDef->SignatureCache->Type->ElementType == SignatureElementType_Var)
				{
					//printf("Resolving a generic member reference!\n");
					type = ((MemberReference*)token->Data)->ParentGenericType;
				}
				else
				{
					//printf("WAS???? ES WAR NICHT GENERIC????\n");
					type = fieldDef->TypeDefinition->File->Assembly->Types[fieldDef->TypeDefinition->TableIndex - 1];
				}
				for (uint32_t index = 0; index < type->FieldCount; index++)
				{
					if (!strcmp(fieldDef->Name, type->Fields[index]->FieldDefinition->Name))
					{
						if (Signature_Equals(fieldDef->Signature, fieldDef->SignatureLength, type->Fields[index]->FieldDefinition->Signature, type->Fields[index]->FieldDefinition->SignatureLength))
						{
							field = type->Fields[index];
							*pFieldIndex = index;
							break;
						}
					}
				}
				if (!field)
				{
					Panic("AppDomain_GetIRFieldFromMetadataToken: Unable to resolve Field");
				}
				//printf("Resolved field to %s of type %s\n", field->FieldDefinition->Name, field->FieldType->TypeDefinition->Name);
			}
			
			break;
		}
		default:
			Panic("AppDomain_GetIRFieldFromMetadataToken: Unknown Table");
			break;
	}
	CLIFile_DestroyMetadataToken(token);
	return field;
}

IRType* AppDomain_GetIRTypeFromMetadataToken(AppDomain* pDomain, IRAssembly* pAssembly, uint32_t pToken, bool_t pClassToken)
{
	MetadataToken* token = NULL;
	if (pClassToken)
	{
		token = CLIFile_ExpandTypeDefRefOrSpecToken(pAssembly->ParentFile, pToken);
	}
	else
	{
		token = CLIFile_ExpandMetadataToken(pAssembly->ParentFile, pToken);
	}
	IRType* type = NULL;
	switch(token->Table)
	{
		case MetadataTable_TypeDefinition:
		{
			TypeDefinition* typeDef = (TypeDefinition*)token->Data;
			/*if (AppDomain_IsStructure(pDomain, typeDef))
			{
				type = IRAssembly_MakePointerType(pAssembly, pAssembly->Types[typeDef->TableIndex - 1]);
			}
			else
			{*/
				type = pAssembly->Types[typeDef->TableIndex - 1];
			//}
			break;
		}
		case MetadataTable_TypeReference:
		{
			if (!((TypeReference*)token->Data)->ResolvedType)
			{
				((TypeReference*)token->Data)->ResolvedType = AppDomain_ResolveTypeReference(pDomain, pAssembly->ParentFile, (TypeReference*)token->Data);
			}
			TypeDefinition* typeDef = ((TypeReference*)token->Data)->ResolvedType;
			/*if (AppDomain_IsStructure(pDomain, typeDef))
			{
				printf("Yep, %s\n", typeDef->Name);
				type = IRAssembly_MakePointerType(pAssembly, typeDef->File->Assembly->Types[typeDef->TableIndex - 1]);
			}
			else
			{*/
				type = typeDef->File->Assembly->Types[typeDef->TableIndex - 1];
			//}
			break;
		}
		case MetadataTable_TypeSpecification:
		{
			SignatureType* sig =  SignatureType_Expand(((TypeSpecification*)token->Data)->Signature, ((TypeSpecification*)token->Data)->File);
			type = AppDomain_GetIRTypeFromSignatureType(pDomain, pAssembly, sig);
			break;
		}
		case MetadataTable_StandAloneSignature:
		{
			Panic("AppDomain_GetIRTypeFromMetadataToken: StandAloneSignatures not yet supported\n");
			break;
		}
		default:
			Panic("AppDomain_GetIRTypeFromMetadataToken: Unknown Table");
			break;
	}
	CLIFile_DestroyMetadataToken(token);
	return type;
}

IRType* AppDomain_GetIRTypeFromElementType(AppDomain* pDomain, ElementType pType)
{
	switch(pType)
	{
		case ElementType_I:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_IntPtr->TableIndex - 1];
		case ElementType_U:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UIntPtr->TableIndex - 1];
		case ElementType_I1:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_SByte->TableIndex - 1];
		case ElementType_I2:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Int16->TableIndex - 1];
		case ElementType_I4:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Int32->TableIndex - 1];
		case ElementType_I8:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Int64->TableIndex - 1];
		case ElementType_U1:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Byte->TableIndex - 1];
		case ElementType_U2:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UInt16->TableIndex - 1];
		case ElementType_U4:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UInt32->TableIndex - 1];
		case ElementType_U8:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_UInt64->TableIndex - 1];
		case ElementType_R4:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Single->TableIndex - 1];
		case ElementType_R8:
			return pDomain->IRAssemblies[0]->Types[pDomain->CachedType___System_Double->TableIndex - 1];
		// This doesn't have a default for a reason.
	}
	return NULL;
}

ElementType AppDomain_GetElementTypeFromIRType(AppDomain* pDomain, IRType* pType)
{
	/*
	ElementType_I1,
	ElementType_U1,
	ElementType_I2,
	ElementType_U2,
	ElementType_I4,
	ElementType_U4,
	ElementType_I8,
	ElementType_U8,
	ElementType_I,
	ElementType_U,
	ElementType_R4,
	ElementType_R8,
	*/
	if (pType->TypeDefinition == pDomain->CachedType___System_Boolean) return ElementType_I1;
	if (pType->TypeDefinition == pDomain->CachedType___System_SByte) return ElementType_I1;
	if (pType->TypeDefinition == pDomain->CachedType___System_Byte) return ElementType_U1;
	if (pType->TypeDefinition == pDomain->CachedType___System_Int16) return ElementType_I2;
	if (pType->TypeDefinition == pDomain->CachedType___System_UInt16) return ElementType_U2;
	if (pType->TypeDefinition == pDomain->CachedType___System_Int32) return ElementType_I4;
	if (pType->TypeDefinition == pDomain->CachedType___System_UInt32) return ElementType_U4;
	if (pType->TypeDefinition == pDomain->CachedType___System_Int64) return ElementType_I8;
	if (pType->TypeDefinition == pDomain->CachedType___System_UInt64) return ElementType_U8;
	if (pType->TypeDefinition == pDomain->CachedType___System_IntPtr || pType->IsPointerType) return ElementType_I;
	if (pType->TypeDefinition == pDomain->CachedType___System_UIntPtr) return ElementType_U;
	if (pType->TypeDefinition == pDomain->CachedType___System_Single) return ElementType_R4;
	if (pType->TypeDefinition == pDomain->CachedType___System_Double) return ElementType_R8;
	//printf("GetElementTypeFromIRType: Unknown %s.%s\n", pType->TypeDefinition->Namespace, pType->TypeDefinition->Name);
	return (ElementType)-1;
}

IRMethod* AppDomain_GetIRMethodFromDefinitionAndSignature(AppDomain* pDomain, IRAssembly* pAssembly, MethodDefinition* pMethodDefinition, SignatureMethodSpecification* pSignatureMethodSpecification)
{
	IRMethod* genericMethod = pMethodDefinition->File->Assembly->Methods[pMethodDefinition->TableIndex - 1];
	if (!genericMethod) genericMethod = IRMethod_Create(pMethodDefinition->File->Assembly, pMethodDefinition);
	IRGenericMethod key;
	memset(&key, 0, sizeof(IRGenericMethod));
	key.GenericMethod = genericMethod;
	key.ParameterCount = pSignatureMethodSpecification->GenericInstGenericArgumentCount;
	for (uint32_t index = 0; index < pSignatureMethodSpecification->GenericInstGenericArgumentCount; ++index)
	{
		if (pSignatureMethodSpecification->GenericInstGenericArguments[index]->ElementType == SignatureElementType_Var) return genericMethod;

		key.Parameters[index] = AppDomain_GetIRTypeFromSignatureType(pDomain, pAssembly, pSignatureMethodSpecification->GenericInstGenericArguments[index]);
	}
	key.ParentType = pAssembly->Types[pMethodDefinition->TypeDefinition->TableIndex - 1];

	IRGenericMethod* lookupMethod = NULL;
	HASH_FIND(HashHandle, pAssembly->GenericMethodsHashTable, &key, offsetof(IRGenericMethod, ImplementationMethod), lookupMethod);
	IRMethod* implementationMethod = NULL;
	if (!lookupMethod)
	{
		implementationMethod = IRMethod_GenericDeepCopy(genericMethod, pAssembly);
		implementationMethod->GenericMethod = IRGenericMethod_Create(key.ParentType, genericMethod, implementationMethod);
		implementationMethod->GenericMethod->ParameterCount = pSignatureMethodSpecification->GenericInstGenericArgumentCount;
		for (uint32_t index = 0; index < pSignatureMethodSpecification->GenericInstGenericArgumentCount; ++index)
		{
			implementationMethod->GenericMethod->Parameters[index] = key.Parameters[index];
		}
		HASH_ADD(HashHandle, pAssembly->GenericMethodsHashTable, GenericMethod, offsetof(IRGenericMethod, ImplementationMethod), implementationMethod->GenericMethod);
		AppDomain_ResolveGenericMethodParameters(pDomain, pAssembly->ParentFile, key.ParentType, implementationMethod);
		implementationMethod->IsGenericImplementation = TRUE;
	}
	else
	{
		implementationMethod = lookupMethod->ImplementationMethod;
	}
	return implementationMethod;
}

bool_t AppDomain_IsStructure(AppDomain* pDomain, TypeDefinition* pTypeDefinition)
{
	if (pTypeDefinition == pDomain->CachedType___System_Boolean) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_Byte) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_Char) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_Double) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_Int16) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_Int32) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_Int64) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_IntPtr) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_SByte) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_Single) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_UInt16) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_UInt32) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_UInt64) return FALSE;
	if (pTypeDefinition == pDomain->CachedType___System_UIntPtr) return FALSE;
	if (pTypeDefinition->TypeOfExtends == TypeDefRefOrSpecType_TypeDefinition)
	{ 
		if (pTypeDefinition->Extends.TypeDefinition == pDomain->CachedType___System_Enum) return TRUE;
		if (pTypeDefinition->Extends.TypeDefinition == pDomain->CachedType___System_ValueType) return TRUE;
		return FALSE;
	}
	if (pTypeDefinition->TypeOfExtends == TypeDefRefOrSpecType_TypeReference)
	{ 
		if (!pTypeDefinition->Extends.TypeReference->ResolvedType)
		{
			pTypeDefinition->Extends.TypeReference->ResolvedType = AppDomain_ResolveTypeReference(pDomain, pDomain->IRAssemblies[0]->ParentFile, pTypeDefinition->Extends.TypeReference);
		}
		if (pTypeDefinition->Extends.TypeReference->ResolvedType == pDomain->CachedType___System_Enum) return TRUE;
		if (pTypeDefinition->Extends.TypeReference->ResolvedType == pDomain->CachedType___System_ValueType) return TRUE;
		return FALSE;
	}
	return FALSE;
}

TypeDefinition* AppDomain_ResolveTypeReference(AppDomain* pDomain, CLIFile* pFile, TypeReference* pTypeReference)
{
	if (pTypeReference->ExportedType)
	{
		switch(pTypeReference->ExportedType->TypeOfImplementation)
		{
			case ImplementationType_File:
			{
				CLIFile* file = NULL;
				for (uint32_t index = 0; index < pDomain->IRAssemblyCount; index++)
				{
					if (!strcmp(pDomain->IRAssemblies[index]->ParentFile->Filename, pTypeReference->ExportedType->Implementation.File->Name))
					{
						file = pDomain->IRAssemblies[index]->ParentFile;
						break;
					}
				}
				if (!file) Panic("Failed to resolve assembly reference for exported type file resolution.");
				for (uint32_t index = 1; index <= file->TypeDefinitionCount; ++index)
				{
					if (!strcmp(file->TypeDefinitions[index].Name , pTypeReference->Name) &&
						!strcmp(file->TypeDefinitions[index].Namespace, pTypeReference->Namespace))
					{
						return &file->TypeDefinitions[index];
					}
				}
				if (!pTypeReference->ResolvedType) Panic("Failed to resolve type reference through assembly reference.");
				break;
			}
			case ImplementationType_AssemblyReference:
			{
				AssemblyReference* asmRef = pTypeReference->ExportedType->Implementation.AssemblyReference;
				CLIFile* file = NULL;
				for (uint32_t index = 0; index < pDomain->IRAssemblyCount; index++)
				{
					if (!strcmp(pDomain->IRAssemblies[index]->ParentFile->AssemblyDefinitions[1].Name, asmRef->Name))
					{
						file = pDomain->IRAssemblies[index]->ParentFile;
						break;
					}
				}
				if (!file) Panic("Failed to resolve assembly reference for exported type assembly reference resolution.");
				for (uint32_t index = 1; index <= file->TypeDefinitionCount; ++index)
				{
					if (!strcmp(file->TypeDefinitions[index].Name , pTypeReference->Name) &&
						!strcmp(file->TypeDefinitions[index].Namespace, pTypeReference->Namespace))
					{
						return &file->TypeDefinitions[index];
					}
				}
				if (!pTypeReference->ResolvedType) Panic("Failed to resolve type reference through assembly reference.");
				break;
			}
			default: Panic("Unhandled type reference resolution through exported type"); break;
		}
	}
	else
	{
		switch (pTypeReference->TypeOfResolutionScope)
		{
			case ResolutionScopeType_AssemblyReference:
			{
				AssemblyReference* asmRef = pTypeReference->ResolutionScope.AssemblyReference;
				CLIFile* file = NULL;
				for (uint32_t index = 0; index < pDomain->IRAssemblyCount; index++)
				{
					if (!strcmp(pDomain->IRAssemblies[index]->ParentFile->AssemblyDefinitions[1].Name, asmRef->Name))
					{
						file = pDomain->IRAssemblies[index]->ParentFile;
						break;
					}
				}
				if (!file) Panic("Failed to resolve assembly reference for type reference resolution.");
				for (uint32_t index = 1; index <= file->TypeDefinitionCount; ++index)
				{
					if (!strcmp(file->TypeDefinitions[index].Name , pTypeReference->Name) &&
						!strcmp(file->TypeDefinitions[index].Namespace, pTypeReference->Namespace))
					{
						return &file->TypeDefinitions[index];
					}
				}
				Panic("Failed to resolve type reference through assembly reference.");
				break;
			}
			case ResolutionScopeType_TypeReference:
			{
				TypeDefinition* resolvedTypeDef = AppDomain_ResolveTypeReference(pDomain, pFile, pTypeReference->ResolutionScope.TypeReference);
				for (uint32_t index = 0; index < resolvedTypeDef->NestedClassCount; index++)
				{
					if (!strcmp(resolvedTypeDef->NestedClasses[index]->Nested->Name, pTypeReference->Name) &&
						!strcmp(resolvedTypeDef->NestedClasses[index]->Nested->Namespace, pTypeReference->Namespace))
					{
						return resolvedTypeDef->NestedClasses[index]->Nested;
					}
				}
				Panic("Failed to resolve type reference through type reference.");
				break;
			}
			default:
				Panic("Unhandled type reference resolution");
				break;
		}
	}

	Panic("Unhandled type reference resolution");
	return NULL;
}

void AppDomain_ResolveMemberReference(AppDomain* pDomain, CLIFile* pFile, MemberReference* pMemberReference)
{
	if (pMemberReference->Resolved.Field || pMemberReference->Resolved.MethodDefinition) return;

	switch (pMemberReference->TypeOfParent)
	{
		case MemberRefParentType_TypeDefinition:
		{
			TypeDefinition* typeDef = pMemberReference->Parent.TypeDefinition;
			bool_t isField = pMemberReference->Signature[0] == 0x06;
			if (isField)
			{
				for (uint32_t i2 = 0; i2 < typeDef->FieldListCount; ++i2)
				{
					if (!strcmp(typeDef->FieldList[i2].Name, pMemberReference->Name))
					{
						pMemberReference->TypeOfResolved = FieldOrMethodDefType_Field;
						pMemberReference->Resolved.Field = &typeDef->FieldList[i2];
						break;
					}
				}
				if (!pMemberReference->Resolved.Field) Panic("Failed to resolve member reference through type definition for field.");
			}
			else
			{
				//printf("Searching for %s, from 0x%x\n", pMemberReference->Name, (unsigned int)typeDef);
				for (uint32_t i2 = 0; i2 < typeDef->MethodDefinitionListCount; ++i2)
				{
					//printf("Checking %s.%s.%s\n", typeDef->Namespace, typeDef->Name, typeDef->MethodDefinitionList[i2].Name);
					if (!typeDef->MethodDefinitionList[i2].SignatureCache)
					{
						typeDef->MethodDefinitionList[i2].SignatureCache = MethodSignature_Expand(typeDef->MethodDefinitionList[i2].Signature, typeDef->File);
					}
					if (!pMemberReference->MethodSignatureCache)
					{
						pMemberReference->MethodSignatureCache = MethodSignature_Expand(pMemberReference->Signature, pFile);
					}

					if (!strcmp(typeDef->MethodDefinitionList[i2].Name, pMemberReference->Name) &&
						MethodSignature_Compare(pDomain, typeDef->File->Assembly, typeDef->MethodDefinitionList[i2].SignatureCache, pFile->Assembly, pMemberReference->MethodSignatureCache))
					{
						pMemberReference->TypeOfResolved = FieldOrMethodDefType_MethodDefinition;
						pMemberReference->Resolved.MethodDefinition = &typeDef->MethodDefinitionList[i2];
						break;
					}
				}
				if (!pMemberReference->Resolved.MethodDefinition)
				{
					printf("Name = %s.%s.%s\n", typeDef->Namespace, typeDef->Name, pMemberReference->Name);
					Panic("Failed to resolve member reference through type definition for method definition.");
				}
			}
			break;
		}
		case MemberRefParentType_TypeReference:
		{
			if (!pMemberReference->Parent.TypeReference->ResolvedType)
			{
				pMemberReference->Parent.TypeReference->ResolvedType = AppDomain_ResolveTypeReference(pDomain, pFile, pMemberReference->Parent.TypeReference);
			}
			TypeDefinition* typeDef = pMemberReference->Parent.TypeReference->ResolvedType;
			bool_t isField = pMemberReference->Signature[0] == 0x06;
			if (isField)
			{
				for (uint32_t i2 = 0; i2 < typeDef->FieldListCount; ++i2)
				{
					if (!strcmp(typeDef->FieldList[i2].Name, pMemberReference->Name))
					{
						pMemberReference->TypeOfResolved = FieldOrMethodDefType_Field;
						pMemberReference->Resolved.Field = &typeDef->FieldList[i2];
						break;
					}
				}
				if (!pMemberReference->Resolved.Field) Panic("Failed to resolve member reference through type reference for field.");
			}
			else
			{
				//printf("Searching for %s, from 0x%x\n", pMemberReference->Name, (unsigned int)typeDef);
				for (uint32_t i2 = 0; i2 < typeDef->MethodDefinitionListCount; ++i2)
				{
					//printf("Checking %s.%s.%s\n", typeDef->Namespace, typeDef->Name, typeDef->MethodDefinitionList[i2].Name);
					if (!typeDef->MethodDefinitionList[i2].SignatureCache)
					{
						typeDef->MethodDefinitionList[i2].SignatureCache = MethodSignature_Expand(typeDef->MethodDefinitionList[i2].Signature, typeDef->File);
					}
					if (!pMemberReference->MethodSignatureCache)
					{
						pMemberReference->MethodSignatureCache = MethodSignature_Expand(pMemberReference->Signature, pFile);
					}

					if (!strcmp(typeDef->MethodDefinitionList[i2].Name, pMemberReference->Name) &&
						MethodSignature_Compare(pDomain, typeDef->File->Assembly, typeDef->MethodDefinitionList[i2].SignatureCache, pFile->Assembly, pMemberReference->MethodSignatureCache))
					{
						pMemberReference->TypeOfResolved = FieldOrMethodDefType_MethodDefinition;
						pMemberReference->Resolved.MethodDefinition = &typeDef->MethodDefinitionList[i2];
						break;
					}
				}
				if (!pMemberReference->Resolved.MethodDefinition)
				{
					printf("Name = %s.%s.%s\n", typeDef->Namespace, typeDef->Name, pMemberReference->Name);
					Panic("Failed to resolve member reference through type reference for method definition.");
				}
			}
			break;
		}
		case MemberRefParentType_TypeSpecification:
		{
			TypeSpecification* typeSpec = pMemberReference->Parent.TypeSpecification;
			SignatureType* sig =  SignatureType_Expand(typeSpec->Signature, typeSpec->File);
			IRType* tp = AppDomain_GetIRTypeFromSignatureType(pDomain, pFile->Assembly, sig);
			TypeDefinition* typeDef = tp->TypeDefinition;
			bool_t isField = pMemberReference->Signature[0] == 0x06;
			if (isField)
			{
				for (uint32_t i2 = 0; i2 < typeDef->FieldListCount; ++i2)
				{
					if (!strcmp(typeDef->FieldList[i2].Name, pMemberReference->Name))
					{
						pMemberReference->TypeOfResolved = FieldOrMethodDefType_Field;
						pMemberReference->Resolved.Field = &typeDef->FieldList[i2];

						if (!typeDef->FieldList[i2].SignatureCache) typeDef->FieldList[i2].SignatureCache = FieldSignature_Expand(typeDef->FieldList[i2].Signature, typeDef->File);
						if (typeDef->FieldList[i2].SignatureCache->Type->ElementType == SignatureElementType_Var)
						{
							if (!(typeDef->FieldList[i2].Flags & FieldAttributes_Static))
							{
								pMemberReference->ParentGenericType = tp;
							}
							else
							{
								Panic("Not currently supported (Pwetty please support me?)");
							}
						}
						break;
					}
				}
				if (!pMemberReference->Resolved.Field) Panic("Failed to resolve member reference through type specification for field.");
			}
			else
			{
				//printf("Searching for %s, from 0x%x\n", pMemberReference->Name, (unsigned int)typeDef);
				for (uint32_t i2 = 0; i2 < typeDef->MethodDefinitionListCount; ++i2)
				{
					//printf("Checking %s.%s.%s\n", typeDef->Namespace, typeDef->Name, typeDef->MethodDefinitionList[i2].Name);
					if (!typeDef->MethodDefinitionList[i2].SignatureCache)
					{
						typeDef->MethodDefinitionList[i2].SignatureCache = MethodSignature_Expand(typeDef->MethodDefinitionList[i2].Signature, typeDef->File);
					}
					if (!pMemberReference->MethodSignatureCache)
					{
						pMemberReference->MethodSignatureCache = MethodSignature_Expand(pMemberReference->Signature, pFile);
					}

					if (!strcmp(typeDef->MethodDefinitionList[i2].Name, pMemberReference->Name) &&
						MethodSignature_Compare(pDomain, typeDef->File->Assembly, typeDef->MethodDefinitionList[i2].SignatureCache, pFile->Assembly, pMemberReference->MethodSignatureCache))
					{
						pMemberReference->TypeOfResolved = FieldOrMethodDefType_MethodDefinition;
						pMemberReference->Resolved.MethodDefinition = &typeDef->MethodDefinitionList[i2];
						if (typeDef->GenericParameterCount)
						{
							for (uint32_t i42 = 0; i42 < tp->MethodCount; i42++)
							{
								if (!strcmp(pMemberReference->Name, tp->Methods[i42]->MethodDefinition->Name) &&
									MethodSignature_Compare(pDomain, typeDef->File->Assembly, tp->Methods[i42]->MethodDefinition->SignatureCache, pFile->Assembly, pMemberReference->MethodSignatureCache))
								{
									pMemberReference->Resolved.ResolvedGenericMethodImplementation = tp->Methods[i42];
									break;
								}
							}
							if (!pMemberReference->Resolved.ResolvedGenericMethodImplementation) Panic("Resolution failed here!!!");
							pMemberReference->ParentGenericType = tp;
						}
						break;
					}
				}
				if (!pMemberReference->Resolved.MethodDefinition) Panic("Failed to resolve member reference through type specification for method definition.");
			}
			//Panic("Member reference resolution through TypeSpec not yet supported!\n");
			break;
		}
		default: Panic("Unhandled member reference resolution"); break;
	}
}

void AppDomain_ResolveGenericTypeParameters(AppDomain* pDomain, CLIFile* pFile, IRType* pType)
{
	//printf("Resolving the generic type parameters for a %s.%s and it has %i fields\n", pType->TypeDefinition->Namespace, pType->TypeDefinition->Name, (int)pType->FieldCount);
	if (!pType->IsGeneric)
		return;

	for(uint32_t index = 0; index < pType->FieldCount; index++)
	{
		IRField* field = pType->Fields[index];
		field->ParentType = pType;
		if (field->FieldType->IsGenericParameter)
		{
			field->FieldType = pType->GenericType->Parameters[field->FieldType->GenericParameterIndex];
			//printf("Well, we got here. 0x%X, %s of type 0x%X %s, 0x%X\n", (unsigned int)field, field->FieldDefinition->Name, (unsigned int)field->FieldType, field->FieldType->TypeDefinition->Name, (unsigned int)pType);
		}
		else if (field->FieldType->ArrayType && field->FieldType->ArrayType->ElementType->IsGenericParameter)
		{
			field->FieldType->ArrayType->ElementType = pType->GenericType->Parameters[field->FieldType->ArrayType->ElementType->GenericParameterIndex];
		}
		else if (field->FieldType->IsGeneric && !field->FieldType->IsGenericInstantiation)
		{
			IRType* fieldType = field->FieldType;
			IRGenericType key;
			memset(&key, 0, sizeof(IRGenericType));
			key.GenericType = pType->GenericType->GenericType;
			key.ParameterCount = pType->GenericType->ParameterCount;
			for (uint32_t index = 0; index < pType->GenericType->ParameterCount; ++index)
			{
				if (fieldType->GenericType->Parameters[index]->IsGenericParameter)
				{
					key.Parameters[index] = pType->GenericType->Parameters[fieldType->GenericType->Parameters[index]->GenericParameterIndex];
				}
				else
				{
					key.Parameters[index] = fieldType->GenericType->Parameters[index];
				}
			}
			IRGenericType* lookupType = NULL;
			HASH_FIND(HashHandle, pFile->Assembly->GenericTypesHashTable, &key, offsetof(IRGenericType, ImplementationType), lookupType);
			if (!lookupType)
			{
				IRType* implementationType = IRType_GenericDeepCopy(fieldType, pFile->Assembly);
				implementationType->GenericType = IRGenericType_Create(fieldType, implementationType);
				implementationType->GenericType->ParameterCount = pType->GenericType->ParameterCount;
				for (uint32_t index = 0; index < pType->GenericType->ParameterCount; ++index)
				{
					if (fieldType->GenericType->Parameters[index]->IsGenericParameter)
					{
						implementationType->GenericType->Parameters[index] = pType->GenericType->Parameters[fieldType->GenericType->Parameters[index]->GenericParameterIndex];
					}
					else
					{
						implementationType->GenericType->Parameters[index] = fieldType->GenericType->Parameters[index];
					}
					//printf("ResolveGenericTypeParameters, GenericInstantiation Type %s.%s, Param %u Type %s.%s\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index, implementationType->GenericType->Parameters[index]->TypeDefinition->Namespace, implementationType->GenericType->Parameters[index]->TypeDefinition->Name);
				}
				field->FieldType = implementationType;
				HASH_ADD(HashHandle, pFile->Assembly->GenericTypesHashTable, GenericType, offsetof(IRGenericType, ImplementationType), implementationType->GenericType);
				IRType_GenericDeepCopy_Finalize(fieldType, pFile->Assembly, implementationType);
				AppDomain_ResolveGenericTypeParameters(pDomain, pFile, implementationType);
				implementationType->IsGenericInstantiation = TRUE;
			}
			else
			{
				field->FieldType = lookupType->ImplementationType;
			}
		}
	}

	for(uint32_t index = 0; index < pType->MethodCount; index++)
	{
		IRMethod* method = pType->Methods[index];
		AppDomain_ResolveGenericMethodParameters(pDomain, pFile, pType, method);
	}

	IRInterfaceImpl* removed = NULL;
	for (IRInterfaceImpl* iterator = pType->InterfaceTable, *iteratorNext = NULL; iterator; iterator = iteratorNext)
	{
		iteratorNext = (IRInterfaceImpl*)iterator->HashHandle.next;
		if (iterator->InterfaceType->IsGeneric && !iterator->InterfaceType->IsGenericInstantiation)
		{
			HASH_DELETE(HashHandle, pType->InterfaceTable, iterator);
			HASH_ADD(HashHandle, removed, InterfaceType, sizeof(void*), iterator);
		}
	}
	for (IRInterfaceImpl* iterator = removed, *iteratorNext = NULL; iterator; iterator = iteratorNext)
	{
		iteratorNext = (IRInterfaceImpl*)iterator->HashHandle.next;
		
		IRType* interfaceType = iterator->InterfaceType;
		IRGenericType key;
		memset(&key, 0, sizeof(IRGenericType));
		key.GenericType = pType->GenericType->GenericType;
		key.ParameterCount = pType->GenericType->ParameterCount;
		for (uint32_t index = 0; index < pType->GenericType->ParameterCount; ++index)
		{
			if (interfaceType->GenericType->Parameters[index]->IsGenericParameter)
			{
				key.Parameters[index] = pType->GenericType->Parameters[interfaceType->GenericType->Parameters[index]->GenericParameterIndex];
			}
			else
			{
				key.Parameters[index] = interfaceType->GenericType->Parameters[index];
			}
		}
		IRGenericType* lookupType = NULL;
		HASH_FIND(HashHandle, pFile->Assembly->GenericTypesHashTable, &key, offsetof(IRGenericType, ImplementationType), lookupType);
		if (!lookupType)
		{
			IRType* implementationType = IRType_GenericDeepCopy(interfaceType, pFile->Assembly);
			implementationType->GenericType = IRGenericType_Create(interfaceType, implementationType);
			implementationType->GenericType->ParameterCount = pType->GenericType->ParameterCount;
			for (uint32_t index = 0; index < pType->GenericType->ParameterCount; ++index)
			{
				if (interfaceType->GenericType->Parameters[index]->IsGenericParameter)
				{
					implementationType->GenericType->Parameters[index] = pType->GenericType->Parameters[interfaceType->GenericType->Parameters[index]->GenericParameterIndex];
				}
				else
				{
					implementationType->GenericType->Parameters[index] = interfaceType->GenericType->Parameters[index];
				}
				//printf("ResolveGenericTypeParameters, GenericInstantiation Interface Type %s.%s, Param %u Type %s.%s\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index, implementationType->GenericType->Parameters[index]->TypeDefinition->Namespace, implementationType->GenericType->Parameters[index]->TypeDefinition->Name);
			}
			iterator->InterfaceType = implementationType;
			HASH_ADD(HashHandle, pFile->Assembly->GenericTypesHashTable, GenericType, offsetof(IRGenericType, ImplementationType), implementationType->GenericType);
			IRType_GenericDeepCopy_Finalize(interfaceType, pFile->Assembly, implementationType);
			AppDomain_ResolveGenericTypeParameters(pDomain, pFile, implementationType);
			implementationType->IsGenericInstantiation = TRUE;
		}
		else
		{
			iterator->InterfaceType = lookupType->ImplementationType;
		}

		HASH_DELETE(HashHandle, removed, iterator);
		HASH_ADD(HashHandle, pType->InterfaceTable, InterfaceType, sizeof(void*), iterator);
	}
}

void AppDomain_ResolveGenericMethodParametersInternal(AppDomain* pDomain, CLIFile* pFile, IRType* pType, IRMethod* pMethod, IRType** pResolvingType)
{
	if (pType)
	{
		//printf("Resolving for %s.%s.%s\n", pType->TypeDefinition->Namespace, pType->TypeDefinition->Name, pMethod->MethodDefinition->Name);
	}
	else
	{
		Panic("This shouldn't be occuring yet!");
	}
	IRType* type = *pResolvingType;
	if (type->IsGenericParameter)
	{
		if (type->IsGenericParameterFromParentType)
		{
			if (!pType || !pType->GenericType) Panic("This shouldn't happen ever!");
			//printf("We're in here, %X\n", (unsigned int)pType->GenericType);
			*pResolvingType = pType->GenericType->Parameters[type->GenericParameterIndex];
		}
		else
		{
			if (!pMethod || !pMethod->GenericMethod) Panic("This better not happen ever!");
			//printf("This time we're in here, %X\n", (unsigned int)pMethod->GenericMethod);
			*pResolvingType = pMethod->GenericMethod->Parameters[type->GenericParameterIndex];
		}
		return;
	}
	else
	{
		//printf("And finally this time in here\n");
	}
	IRGenericType key;
	memset(&key, 0, sizeof(IRGenericType));
	key.GenericType = type;
	key.ParameterCount = type->GenericParameterCount;
	for (uint32_t index = 0; index < type->GenericParameterCount; ++index)
	{
		//printf("GenericType = %X\n", (unsigned int)type->GenericType);
		if (!type->GenericType->Parameters[index]->IsGenericInstantiation)
		{
			if (type->GenericType->Parameters[index]->IsGenericParameter)
			{
				//printf("IsGenericParameter\n");
				if (type->GenericType->Parameters[index]->IsGenericParameterFromParentType)
				{
					if (!pType) Panic("This shouldn't happen!");
					key.Parameters[index] = pType->GenericType->Parameters[type->GenericType->Parameters[index]->GenericParameterIndex];
				}
				else
				{
					if (!pMethod->GenericMethod) Panic("This better not happen!");
					key.Parameters[index] = pMethod->GenericMethod->Parameters[type->GenericType->Parameters[index]->GenericParameterIndex];
				}
			}
			else
			{
				//printf("NotGenericParameter\n");
				key.Parameters[index] = type->GenericType->Parameters[index];
			}
		}
	}
	for (uint32_t i = 0; i < type->GenericParameterCount; i++)
	{
		if (key.Parameters[i]->IsGenericParameter)
		{
			*pResolvingType = type->GenericType->GenericType;
			return;
		}
	}
	IRGenericType* lookupType = NULL;
	HASH_FIND(HashHandle, pFile->Assembly->GenericTypesHashTable, &key, offsetof(IRGenericType, ImplementationType), lookupType);
	if (!lookupType)
	{
		IRType* implementationType = IRType_GenericDeepCopy(type, pFile->Assembly);
		implementationType->GenericType = IRGenericType_Create(type, implementationType);
		implementationType->GenericType->ParameterCount = pType->GenericType->ParameterCount;
		for (uint32_t index = 0; index < pType->GenericType->ParameterCount; ++index)
		{
			if (type->GenericType->Parameters[index]->IsGenericParameter)
			{
				if (type->IsGenericParameterFromParentType)
				{
					implementationType->GenericType->Parameters[index] = pType->GenericType->Parameters[type->GenericParameterIndex];
					//if (pType->GenericType->Parameters[type->GenericParameterIndex]->IsGenericParameter)
					//{
					//	printf("ResolveGenericMethodParameters from Type, GenericInstantiation Type %s.%s, Param %u Still Generic\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index);
					//}
					//else
					//{
					//	printf("ResolveGenericMethodParameters from Type, GenericInstantiation Type %s.%s, Param %u Type %s.%s\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index, implementationType->GenericType->Parameters[index]->TypeDefinition->Namespace, implementationType->GenericType->Parameters[index]->TypeDefinition->Name);
					//}
				}
				else
				{
					implementationType->GenericType->Parameters[index] = pMethod->GenericMethod->Parameters[type->GenericParameterIndex];
					//if (pMethod->GenericMethod->Parameters[type->GenericParameterIndex]->IsGenericParameter)
					//{
					//	printf("ResolveGenericMethodParameters from Method, GenericInstantiation Type %s.%s, Param %u Still Generic\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index);
					//}
					//else
					//{
					//	printf("ResolveGenericMethodParameters from Method, GenericInstantiation Type %s.%s, Param %u Type %s.%s\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index, implementationType->GenericType->Parameters[index]->TypeDefinition->Namespace, implementationType->GenericType->Parameters[index]->TypeDefinition->Name);
					//}
				}
			}
			else
			{
				implementationType->GenericType->Parameters[index] = type->GenericType->Parameters[index];
				//printf("ResolveGenericMethodParameters, GenericInstantiation Type %s.%s, Param %u was already Type %s.%s\n", implementationType->TypeDefinition->Namespace, implementationType->TypeDefinition->Name, (unsigned int)index, implementationType->GenericType->Parameters[index]->TypeDefinition->Namespace, implementationType->GenericType->Parameters[index]->TypeDefinition->Name);
			}
		}
		*pResolvingType = implementationType;
		HASH_ADD(HashHandle, pFile->Assembly->GenericTypesHashTable, GenericType, offsetof(IRGenericType, ImplementationType), implementationType->GenericType);
		IRType_GenericDeepCopy_Finalize(type, pFile->Assembly, implementationType);
		AppDomain_ResolveGenericTypeParameters(pDomain, pFile, implementationType);
		implementationType->IsGenericInstantiation = TRUE;
	}
	else
	{
		*pResolvingType = lookupType->ImplementationType;
	}
}

void AppDomain_ResolveGenericMethodParametersFinalize(AppDomain* pDomain, CLIFile* pFile, IRType* pType, IRMethod* pMethod)
{
	if (!pType || !pMethod) Panic("Weoops...");

	for (uint32_t index = 0; index < pMethod->LocalVariableCount; index++)
	{
		IRLocalVariable* local = pMethod->LocalVariables[index];
		if (local->VariableType->ArrayType && local->VariableType->ArrayType->ElementType->IsGeneric)
		{
			AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, &local->VariableType->ArrayType->ElementType);
		}
		else if (local->VariableType->IsGenericParameter || (local->VariableType->IsGeneric && !local->VariableType->IsGenericInstantiation))
		{
			AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, &local->VariableType);
		}
	}

	for (uint32_t index = 0; index < pMethod->IRCodesCount; index++)
	{
		IRInstruction* instruction = pMethod->IRCodes[index];
		switch (instruction->Opcode)
		{
			case IROpcode_Dup:
			case IROpcode_Pop:
			case IROpcode_Load_Indirect:
			case IROpcode_Store_Indirect:
			case IROpcode_CastClass:
			case IROpcode_IsInst:
			case IROpcode_Unbox:
			case IROpcode_UnboxAny:
			case IROpcode_Box:
			case IROpcode_Allocate_Local:
			case IROpcode_SizeOf:
			case IROpcode_Call_Virtual:
			case IROpcode_Call_Constrained:
			case IROpcode_Load_VirtualFunction:
			case IROpcode_Load_Token:
			case IROpcode_RefAnyVal:
				if ((((IRType*)instruction->Arg1)->IsGeneric && !((IRType*)instruction->Arg1)->IsGenericInstantiation) ||
					((IRType*)instruction->Arg1)->IsGenericParameter)
				{
					AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, (IRType**)&instruction->Arg1);
				}
				break;
			case IROpcode_New_Array:
				if ((((IRType*)instruction->Arg1)->IsGeneric && !((IRType*)instruction->Arg1)->IsGenericInstantiation) ||
					((IRType*)instruction->Arg1)->IsGenericParameter)
				{
					//printf("type: %X, %s, %i\n", (unsigned int)instruction->Arg1, ((IRType*)instruction->Arg1)->TypeDefinition->Name, (int)((IRType*)instruction->Arg1)->GenericParameterIndex);
					AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, (IRType**)&instruction->Arg1);
					//printf("type: %X, %s, %i\n", (unsigned int)instruction->Arg1, ((IRType*)instruction->Arg1)->TypeDefinition->Name, (int)((IRType*)instruction->Arg1)->GenericParameterIndex);
				}
				break;
			case IROpcode_New_Object:
			case IROpcode_Jump:
			case IROpcode_Call_Absolute:
			case IROpcode_Call_Internal:
			case IROpcode_Load_Function:
				if (ILDecomposition_MethodUsesGenerics((IRMethod*)instruction->Arg1))
				{
					Panic("Deal with this later");
				}
				break;
			case IROpcode_Load_StaticField:
			case IROpcode_Load_StaticFieldAddress:
			case IROpcode_Store_StaticField:
				if ((((IRField*)instruction->Arg1)->FieldType->IsGeneric && !((IRField*)instruction->Arg1)->FieldType->IsGenericInstantiation) ||
					((IRField*)instruction->Arg1)->FieldType->IsGenericParameter)
				{
					Panic("Deal with this later");
				}
				break;
			case IROpcode_Load_Object:
			case IROpcode_Store_Object:
			case IROpcode_Copy_Object:
			case IROpcode_Load_Field:
			case IROpcode_Load_FieldAddress:
			case IROpcode_Store_Field:
			case IROpcode_Load_Element:
			case IROpcode_Load_ElementAddress:
			case IROpcode_Store_Element:
			case IROpcode_Initialize_Object:
			case IROpcode_MkRefAny:
				if ((((IRType*)instruction->Arg1)->IsGeneric && !((IRType*)instruction->Arg1)->IsGenericInstantiation) ||
					((IRType*)instruction->Arg1)->IsGenericParameter)
				{
					AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, (IRType**)&instruction->Arg1);
				}
				if ((((IRType*)instruction->Arg2)->IsGeneric && !((IRType*)instruction->Arg2)->IsGenericInstantiation) ||
					((IRType*)instruction->Arg2)->IsGenericParameter)
				{
					AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, (IRType**)&instruction->Arg2);
				}
				break;
			case IROpcode_Branch:
				if ((((IRType*)instruction->Arg3)->IsGeneric && !((IRType*)instruction->Arg3)->IsGenericInstantiation) ||
					((IRType*)instruction->Arg3)->IsGenericParameter)
				{
					AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, (IRType**)&instruction->Arg3);
				}
				if ((((IRType*)instruction->Arg4)->IsGeneric && !((IRType*)instruction->Arg4)->IsGenericInstantiation) ||
					((IRType*)instruction->Arg4)->IsGenericParameter)
				{
					AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, (IRType**)&instruction->Arg4);
				}
				break;
			default: break;
		}
	}
}

void AppDomain_ResolveGenericMethodParameters(AppDomain* pDomain, CLIFile* pFile, IRType* pType, IRMethod* pMethod)
{
	if(pMethod->IsGeneric && !pMethod->IsGenericImplementation)
	{
		//printf("Delaying generic parameter resolution of %s.%s.%s\n", pMethod->MethodDefinition->TypeDefinition->Namespace, pMethod->MethodDefinition->TypeDefinition->Name, pMethod->MethodDefinition->Name);
		return;
	}

	if (pMethod->Returns &&
		((pMethod->ReturnType->IsGeneric && !pMethod->ReturnType->IsGenericInstantiation) || pMethod->ReturnType->IsGenericParameter))
	{
		AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, &pMethod->ReturnType);
	}

	uint32_t startIndex = 0;
	if (pType->IsGeneric && !(pMethod->MethodDefinition->Flags & MethodAttributes_Static))
	{
		if (pMethod->Parameters[0]->Type->IsGeneric)
		{
			pMethod->Parameters[0]->Type = pType;
		}
		startIndex = 1;
	}
	for (uint32_t index = startIndex; index < pMethod->ParameterCount; index++)
	{
		IRParameter* param = pMethod->Parameters[index];
		if ((param->Type->IsGeneric && !param->Type->IsGenericInstantiation) || param->Type->IsGenericParameter)
		{
			//printf("IsGenericParameter1\n");
			AppDomain_ResolveGenericMethodParametersInternal(pDomain, pFile, pType, pMethod, &param->Type);
			//printf("IsGenericParameter2\n");
		}
	}
}


TypeDefinition* AppDomain_GetTypeDefByCanonicalName(AppDomain* pDomain, IRAssembly* pAssembly, char* pCanonicalName)
{
	TypeDefinition* retType = NULL;
	char* assemblyNameStart;
	char* typeNameStart = NULL;
	bool_t hasAssembly = FALSE;
	uint32_t strLen = strlen(pCanonicalName);
	for (uint32_t i = 0; i < strLen; i++)
	{
		if (pCanonicalName[i] == ',')
		{
			hasAssembly = TRUE;
			assemblyNameStart = &pCanonicalName[i + 2];
			break;
		}
		else if (pCanonicalName[i] == '.')
		{
			typeNameStart = &pCanonicalName[i + 1];
		}
	}
	if (hasAssembly)
	{
		// locate the assembly and look through the types in it.
		Panic("We don't currently support getting the type def by a canonical name if it includes the assembly name!");
	}
	else
	{
		// look first through the current assembly, then through mscorlib.
		for (uint32_t i = 1; i <= pAssembly->ParentFile->TypeDefinitionCount; i++)
		{
			if (!typeNameStart) // it's in the global namespace.
			{
				if (!*pAssembly->ParentFile->TypeDefinitions[i].Namespace && !memcmp(pCanonicalName, pAssembly->ParentFile->TypeDefinitions[i].Name, strLen))
				{
					retType = &pAssembly->ParentFile->TypeDefinitions[i];
					break;
				}
			}
			else
			{
				if (!memcmp(pCanonicalName, pAssembly->ParentFile->TypeDefinitions[i].Namespace, typeNameStart - pCanonicalName - 1) &&
					!memcmp(typeNameStart, pAssembly->ParentFile->TypeDefinitions[i].Name, strLen - (typeNameStart - pCanonicalName - 1)))
				{
					retType = &pAssembly->ParentFile->TypeDefinitions[i];
					break;
				}
			}
		}

		if (pAssembly != pDomain->IRAssemblies[0]) // ensure we aren't checking corlib twice.
		{
			for (uint32_t i = 1; i <= pDomain->IRAssemblies[0]->ParentFile->TypeDefinitionCount; i++)
			{
				if (!typeNameStart) // it's in the global namespace.
				{
					if (!*pDomain->IRAssemblies[0]->ParentFile->TypeDefinitions[i].Namespace && !memcmp(pCanonicalName, pDomain->IRAssemblies[0]->ParentFile->TypeDefinitions[i].Name, strLen))
					{
						retType = &pDomain->IRAssemblies[0]->ParentFile->TypeDefinitions[i];
						break;
					}
				}
				else
				{
					if (!memcmp(pCanonicalName, pDomain->IRAssemblies[0]->ParentFile->TypeDefinitions[i].Namespace, typeNameStart - pCanonicalName - 1) &&
						!memcmp(typeNameStart, pDomain->IRAssemblies[0]->ParentFile->TypeDefinitions[i].Name, strLen - (typeNameStart - pCanonicalName - 1)))
					{
						retType = &pDomain->IRAssemblies[0]->ParentFile->TypeDefinitions[i];
						break;
					}
				}
			}
		}
	}
	if (!retType)
		Panic("Unable to resolve a type by it's canonical name!");
	return retType;
}
