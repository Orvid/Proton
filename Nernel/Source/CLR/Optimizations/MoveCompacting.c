#include <CLR/Optimizations/MoveCompacting.h>
#include <CLR/IRMethodDumper.h>


void ProcessSourceTypeData(SourceTypeData* sDat, uint32_t** localUseCount, bool_t** addressLoaded, bool_t isAddress)
{
	switch(sDat->Type)
	{
		case SourceType_ArrayElement:
			ProcessSourceTypeData(sDat->Data.ArrayElement.ArraySource, localUseCount, addressLoaded, FALSE);
			ProcessSourceTypeData(sDat->Data.ArrayElement.IndexSource, localUseCount, addressLoaded, FALSE);
			break;
		case SourceType_ArrayElementAddress:
			ProcessSourceTypeData(sDat->Data.ArrayElementAddress.ArraySource, localUseCount, addressLoaded, TRUE);
			ProcessSourceTypeData(sDat->Data.ArrayElementAddress.IndexSource, localUseCount, addressLoaded, FALSE);
			break;
		case SourceType_ArrayLength:
			ProcessSourceTypeData(sDat->Data.ArrayLength.ArraySource, localUseCount, addressLoaded, FALSE);
			break;
		case SourceType_Field:
			ProcessSourceTypeData(sDat->Data.Field.FieldSource, localUseCount, addressLoaded, FALSE);
			break;
		case SourceType_FieldAddress:
			ProcessSourceTypeData(sDat->Data.FieldAddress.FieldSource, localUseCount, addressLoaded, TRUE);
			break;
		case SourceType_Indirect:
			ProcessSourceTypeData(sDat->Data.Indirect.AddressSource, localUseCount, addressLoaded, FALSE);
			break;
			
		case SourceType_Local:
			(*localUseCount)[sDat->Data.LocalVariable.LocalVariableIndex]++;
			if (isAddress)
			{
				(*addressLoaded)[sDat->Data.LocalVariable.LocalVariableIndex] = TRUE;
			}
			break;
		case SourceType_LocalAddress:
			(*localUseCount)[sDat->Data.LocalVariableAddress.LocalVariableIndex]++;
			(*addressLoaded)[sDat->Data.LocalVariableAddress.LocalVariableIndex] = TRUE;
			break;
		default:
			break;
	}
}

void RetargetSources(SourceTypeData* searchingInType, SourceTypeData* lookingFor, SourceTypeData* replaceWith)
{
	if (!memcmp(searchingInType, lookingFor, sizeof(SourceTypeData)))
	{
		*searchingInType = *replaceWith;
		return;
	}
	switch(searchingInType->Type)
	{
		case SourceType_ArrayElement:
			RetargetSources(searchingInType->Data.ArrayElement.ArraySource, lookingFor, replaceWith);
			RetargetSources(searchingInType->Data.ArrayElement.IndexSource, lookingFor, replaceWith);
			break;
		case SourceType_ArrayElementAddress:
			RetargetSources(searchingInType->Data.ArrayElementAddress.ArraySource, lookingFor, replaceWith);
			RetargetSources(searchingInType->Data.ArrayElementAddress.IndexSource, lookingFor, replaceWith);
			break;
		case SourceType_ArrayLength:
			RetargetSources(searchingInType->Data.ArrayLength.ArraySource, lookingFor, replaceWith);
			break;
		case SourceType_Field:
			RetargetSources(searchingInType->Data.Field.FieldSource, lookingFor, replaceWith);
			break;
		case SourceType_FieldAddress:
			RetargetSources(searchingInType->Data.FieldAddress.FieldSource, lookingFor, replaceWith);
			break;
		case SourceType_Indirect:
			RetargetSources(searchingInType->Data.Indirect.AddressSource, lookingFor, replaceWith);
			break;
		default:
			break;
	}

}

bool_t IROptimizer_SourceDataUsesLocalVariable(SourceTypeData* pSourceData, uint32_t pLocalVariableIndex);

void IROptimizer_MoveCompacting(IRMethod* pMethod, IRCodeNode** pNodes, uint32_t pNodesCount)
{

	printf("\n\n\nBEFORE MOVE COMPACTING:\n\n\n");
	IRMethodDumper_Dump(pMethod);


	uint32_t localsCompacted;
//Start:
	localsCompacted = 0;
	uint32_t* localUseCount = (uint32_t*)calloc(1, sizeof(uint32_t) * pMethod->LocalVariableCount);
	IRInstruction** localsAssignedAt = (IRInstruction**)calloc(1, sizeof(IRInstruction*) * pMethod->LocalVariableCount);
	bool_t* addressLoadedOf = (bool_t*)calloc(1, sizeof(bool_t) * pMethod->LocalVariableCount);
	for(uint32_t i = 0; i < pNodesCount; i++)
	{
		for (uint32_t i2 = 0; i2 < pNodes[i]->PhiFunctionsCount; i2++)
		{
			//printf("Ignoring local %u because it's a result\n", (unsigned int)pNodes[i]->PhiFunctions[i2]->Result->LocalVariableIndex);
			addressLoadedOf[pNodes[i]->PhiFunctions[i2]->Result->LocalVariableIndex] = TRUE;
			for (uint32_t i3 = 0; i3 < pNodes[i]->PhiFunctions[i2]->ArgumentsCount; i3++)
			{
				//printf("Ignoring local %u because it's an argument\n", (unsigned int)pNodes[i]->PhiFunctions[i2]->Arguments[i3]->LocalVariableIndex);
				addressLoadedOf[pNodes[i]->PhiFunctions[i2]->Arguments[i3]->LocalVariableIndex] = TRUE;
			}
		}
	}
	uint32_t* origLocalUseCount = localUseCount;
	IRInstruction** origLocalsAssignedAt = localsAssignedAt;
	bool_t* origAddressLoadedOf = addressLoadedOf;
	for (uint32_t i42 = 0; i42 < pNodesCount; i42++)
	{
		uint32_t* localUseCount = (uint32_t*)calloc(1, sizeof(uint32_t) * pMethod->LocalVariableCount);
		IRInstruction** localsAssignedAt = (IRInstruction**)calloc(1, sizeof(IRInstruction*) * pMethod->LocalVariableCount);
		bool_t* addressLoadedOf = (bool_t*)calloc(1, sizeof(bool_t) * pMethod->LocalVariableCount);
		memcpy(localUseCount, origLocalUseCount, sizeof(uint32_t) * pMethod->LocalVariableCount);
		memcpy(localsAssignedAt, origLocalsAssignedAt, sizeof(IRInstruction*) * pMethod->LocalVariableCount);
		memcpy(addressLoadedOf, origAddressLoadedOf, sizeof(bool_t) * pMethod->LocalVariableCount);
		for (uint32_t i = 0; i < pNodes[i42]->InstructionsCount; i++)
		{
			IRInstruction* instr = pMethod->IRCodes[pNodes[i42]->Instructions[i]];
			ProcessSourceTypeData(&instr->Source1, &localUseCount, &addressLoadedOf, FALSE);
			ProcessSourceTypeData(&instr->Source2, &localUseCount, &addressLoadedOf, FALSE);
			ProcessSourceTypeData(&instr->Source3, &localUseCount, &addressLoadedOf, FALSE);
			ProcessSourceTypeData(&instr->Destination, &localUseCount, &addressLoadedOf, FALSE);
			if (instr->Destination.Type == SourceType_Local)
			{
				localUseCount[instr->Destination.Data.LocalVariable.LocalVariableIndex]--;
				if (instr->Opcode == IROpcode_Move)
				{
					localsAssignedAt[instr->Destination.Data.LocalVariable.LocalVariableIndex] = instr;
				}
			}
			for (uint32_t i2 = 0; i2 < instr->SourceArrayLength; i2++)
			{
				ProcessSourceTypeData(&instr->SourceArray[i2], &localUseCount, &addressLoadedOf, FALSE);
			}
		}

		for (uint32_t i = 0; i < pMethod->LocalVariableCount; i++)
		{
			if (addressLoadedOf[i])
			{
				//printf("Skipping %u because it had it's address loaded\n", (unsigned int)i);
				continue;
			}
			if (localsAssignedAt[i])
			{
				bool_t found = FALSE;
				for (uint32_t i2 = 0; i2 < pMethod->LocalVariableCount; i2++)
				{
					if (addressLoadedOf[i2])
					{
						if (IROptimizer_SourceDataUsesLocalVariable(&localsAssignedAt[i]->Source1, i2))
						{
							found = TRUE;
							break;
						}
						if (IROptimizer_SourceDataUsesLocalVariable(&localsAssignedAt[i]->Source2, i2))
						{
							found = TRUE;
							break;
						}
						if (IROptimizer_SourceDataUsesLocalVariable(&localsAssignedAt[i]->Source3, i2))
						{
							found = TRUE;
							break;
						}
						if (IROptimizer_SourceDataUsesLocalVariable(&localsAssignedAt[i]->Destination, i2))
						{
							found = TRUE;
							break;
						}
						for (uint32_t i2 = 0; i2 < localsAssignedAt[i]->SourceArrayLength; i2++)
						{
							if (IROptimizer_SourceDataUsesLocalVariable(&localsAssignedAt[i]->SourceArray[i2], i2))
							{
								found = TRUE;
								break;
							}
						}
						if (found) break;
					}
				}
				if (found) continue;
				if (((localsAssignedAt[i]->Source1.Type == SourceType_ConstantI4 ||
					localsAssignedAt[i]->Source1.Type == SourceType_ConstantI8 ||
					localsAssignedAt[i]->Source1.Type == SourceType_ConstantR4 ||
					localsAssignedAt[i]->Source1.Type == SourceType_ConstantR8 ||
					localsAssignedAt[i]->Source1.Type == SourceType_Null ||
					localsAssignedAt[i]->Source1.Type == SourceType_Local ||
					localsAssignedAt[i]->Source1.Type == SourceType_LocalAddress ||
					localsAssignedAt[i]->Source1.Type == SourceType_Parameter ||
					localsAssignedAt[i]->Source1.Type == SourceType_ParameterAddress) && localUseCount[i] >= 1) || 

					localUseCount[i] == 1
					)
				{
					// we need to retarget it's loads to be from it instead.
					for (uint32_t i2 = 0; i2 < pNodes[i42]->InstructionsCount; i2++)
					{
						IRInstruction* instr = pMethod->IRCodes[pNodes[i42]->Instructions[i2]];
						RetargetSources(&instr->Source1, &localsAssignedAt[i]->Destination, &localsAssignedAt[i]->Source1);
						RetargetSources(&instr->Source2, &localsAssignedAt[i]->Destination, &localsAssignedAt[i]->Source1);
						RetargetSources(&instr->Source3, &localsAssignedAt[i]->Destination, &localsAssignedAt[i]->Source1);
						if (instr->Destination.Type != SourceType_Local || instr->Destination.Data.LocalVariable.LocalVariableIndex != i)
						{
							RetargetSources(&instr->Destination, &localsAssignedAt[i]->Destination, &localsAssignedAt[i]->Source1);
						}
						for (uint32_t i3 = 0; i3 < instr->SourceArrayLength; i3++)
						{
							RetargetSources(&instr->SourceArray[i3], &localsAssignedAt[i]->Destination, &localsAssignedAt[i]->Source1);
						}
					}
					localsAssignedAt[i]->Opcode = IROpcode_Nop;
					localsAssignedAt[i]->Arg1 = NULL;
					localsCompacted++;
				}
			}
		}

		free(localUseCount);
		free(localsAssignedAt);
		free(addressLoadedOf);
	}
	free(origLocalUseCount);
	free(origLocalsAssignedAt);
	free(origAddressLoadedOf);

	printf("\n\n\nAFTER MOVE COMPACTING:\n\n\n");
	IRMethodDumper_Dump(pMethod);

	/*if (localsCompacted)
	{
		printf("Repeating after compacting %u variables\n", (unsigned int)localsCompacted);
		goto Start;
	}
	printf("Not-Repeating after compacting %u variables\n", (unsigned int)localsCompacted);*/
}