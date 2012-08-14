﻿using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace Mernel
{
    public static class Kernel
    {
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Write(string pString);

        private unsafe static void Main()
        {
            // TODO: Implement and test all string constructors
            //char[] test = new char[] { 'T', 'E', 'S', 'T', '\n' }; // TODO: Implement InitializeArray runtime helper support
            //char[] test = new char[5];
            //test[0] = 'T';
            //test[1] = 'E';
            //test[2] = 'S';
            //test[3] = 'T';
            //test[4] = '\n';
            //Kernel.Write(new string(test));
            //Kernel.Write(new string((sbyte*)0x1cdf37));
            //byte[] buf = new byte[3];
            //int bufLen = Buffer.ByteLength(buf);
            //if (bufLen == 3) Kernel.Write(bufLen.ToString());
            //int x = 5;
            //Kernel.Write(x.ToString());
            if (System.Runtime.CompilerServices.RuntimeHelpers.OffsetToStringData == 4) Kernel.Write("Test\n");
        }
    }
}
