#include <Common.h>
#include <System/Console.h>
#include <System/SerialLogger.h>
#include <System/x86/PortIO.h>

#define CONSOLE__BaseMemory          0x000B8000
#define CONSOLE__DefaultColumns      80
#define CONSOLE__DefaultRows         25
#define CONSOLE__DefaultAttributes   0x0F

uint8_t* gConsole_BaseMemory = (uint8_t*)CONSOLE__BaseMemory;
uint8_t gConsole_Columns = CONSOLE__DefaultColumns;
uint8_t gConsole_Rows = CONSOLE__DefaultRows;
uint8_t gConsole_CursorColumn = 0;
uint8_t gConsole_CursorRow = 0;
uint8_t gConsole_Attributes = CONSOLE__DefaultAttributes;

void Console_Startup()
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

	outb(0x3D4, 0x0A);
	outb(0x3D5, 0x20);
	Console_Clear(Console_CreateAttributes(CONSOLE__LightWhite, CONSOLE__DarkBlack));
}

void Console_Shutdown()
{
}

void Console_MoveTo(uint8_t pColumn, uint8_t pRow)
{
    gConsole_CursorColumn = pColumn % gConsole_Columns;
    gConsole_CursorRow = pRow % gConsole_Rows;
}

void Console_MoveToTopLeft() { Console_MoveTo(0, 0); }

void Console_MoveToNextLine()
{
	if (gConsole_CursorRow + 1 >= gConsole_Rows)
	{
		uint32_t lengthToCopyUp = ((gConsole_Rows - 1) * gConsole_Columns) * 2;
		memcpy((char*)gConsole_BaseMemory, ((char*)gConsole_BaseMemory) + (gConsole_Columns * 2), lengthToCopyUp);
		for (uint32_t index = 0; index < gConsole_Columns; ++index)
		{
			*(gConsole_BaseMemory + (((gConsole_Rows - 1) * gConsole_Columns) * 2) + (index * 2)) = ' ';
		}
		gConsole_CursorRow = gConsole_Rows - 1;
	}
	else ++gConsole_CursorRow;
	Console_MoveTo(0, gConsole_CursorRow);
}

void Console_Clear(uint8_t pAttributes)
{
	gConsole_Attributes = pAttributes;
	Console_MoveToTopLeft();

	int32_t index = 0;
	int32_t count = gConsole_Columns * gConsole_Rows;
	uint8_t* cursor = Console_GetCursor();
	while (index < count)
	{
		*cursor = ' ';
		*(cursor + 1) = gConsole_Attributes;
		cursor += 2;
		++index;
	}
}

void Console_WriteCharacter(char pCharacter)
{
    if (pCharacter == '\n')
    {
        Console_MoveToNextLine();
    	SerialLogger_WriteByte('\r');
    	SerialLogger_WriteByte('\n');
    }
    else if (pCharacter != '\r')
    {
	    uint8_t* cursor = Console_GetCursor();
	    *cursor = pCharacter;
	    *(cursor + 1) = gConsole_Attributes;
	    Console_Advance();
    	SerialLogger_WriteByte(pCharacter);
    }
}

void Console_WriteString(const char* pString,
                                uint32_t pLength)
{
    const char* iterator = pString;
    bool_t useLength = pLength > 0;
    while (*iterator)
	{
        Console_WriteCharacter(*iterator);
        ++iterator;
        if (useLength)
        {
            --pLength;
            if (pLength == 0) break;
        }
    }
}

void Console_WriteLine(const char* pString)
{
	Console_WriteString(pString, 0);
	if (gConsole_CursorColumn > 0) Console_MoveToNextLine();
    SerialLogger_WriteByte('\r');
    SerialLogger_WriteByte('\n');
}

uint8_t* Console_GetCursor() { return gConsole_BaseMemory + (((gConsole_CursorRow * gConsole_Columns) + gConsole_CursorColumn) * 2); }

void Console_Advance()
{
	++gConsole_CursorColumn;
	if (gConsole_CursorColumn >= gConsole_Columns)
	{
		Console_MoveToNextLine();
	}
}