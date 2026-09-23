import ghidra.program.model.address.Address;
import ghidra.app.script.GhidraScript;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpAllPropertyThunks extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_property_thunks.txt";

    static final long[][] SIMPLE_PROPS = {
        {0x1f9de16L, 0x62326b0L},
        {0x1ce7ea5L, 0x62326c0L},
        {0x1f9df09L, 0x62326d0L},
        {0x1ce7eb0L, 0x62326e0L},
        {0x1ce7ee1L, 0x6232720L},
        {0x1ce7ee9L, 0x6232730L},
        {0x1ce7f00L, 0x6232740L},
        {0x1ce7f12L, 0x6232760L},
        {0x1ce7f1fL, 0x6232770L},
        {0x1ce7f30L, 0x6232780L},
    };
    static final long[] COMPLEX_NAMES = { 0x1cea5c4L, 0x1cfefadL };

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            for (long[] pair : SIMPLE_PROPS) {
                long nameAddr = pair[0];
                long thunkAddr = pair[1];
                String name = readCString(toAddr(nameAddr));
                byte[] thunkBytes = new byte[16];
                for (int i = 0; i < 16; i++) thunkBytes[i] = getByte(toAddr(thunkAddr).add(i));
                String hex = bytesToHex(thunkBytes);
                String offsetInfo = decodeGetterOffset(thunkBytes);
                out.println(String.format("name=%-20s @0x%x  thunk@0x%x  bytes=[%s]  %s",
                        name, nameAddr, thunkAddr, hex, offsetInfo));
            }
            out.println();
            for (long nameAddr : COMPLEX_NAMES) {
                String name = readCString(toAddr(nameAddr));
                out.println(String.format("complex name=%-20s @0x%x", name, nameAddr));
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }

    private String readCString(Address a) {
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 64; i++) {
                byte b = getByte(a.add(i));
                if (b == 0) break;
                sb.append((char) (b & 0xff));
            }
        } catch (Exception e) {
            return "<err:" + e.getMessage() + ">";
        }
        return sb.toString();
    }

    private String bytesToHex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x ", x & 0xff));
        return sb.toString().trim();
    }

    // Recognize common tiny getter patterns:
    //   8a 87 xx xx xx xx     -> mov al,[rdi+disp32]   (bool/byte field)
    //   0f b6 87 xx xx xx xx  -> movzx eax,byte [rdi+disp32]
    //   8b 87 xx xx xx xx     -> mov eax,[rdi+disp32]  (int field)
    //   48 8b 87 xx xx xx xx  -> mov rax,[rdi+disp32]  (ptr/int64 field)
    private String decodeGetterOffset(byte[] b) {
        try {
            if ((b[0] & 0xff) == 0x8a && (b[1] & 0xff) == 0x87) {
                int off = leInt(b, 2);
                return "GETTER byte-field offset=0x" + Integer.toHexString(off);
            }
            if ((b[0] & 0xff) == 0x0f && (b[1] & 0xff) == 0xb6 && (b[2] & 0xff) == 0x87) {
                int off = leInt(b, 3);
                return "GETTER movzx byte-field offset=0x" + Integer.toHexString(off);
            }
            if ((b[0] & 0xff) == 0x8b && (b[1] & 0xff) == 0x87) {
                int off = leInt(b, 2);
                return "GETTER int32-field offset=0x" + Integer.toHexString(off);
            }
            if ((b[0] & 0xff) == 0x48 && (b[1] & 0xff) == 0x8b && (b[2] & 0xff) == 0x87) {
                int off = leInt(b, 3);
                return "GETTER int64/ptr-field offset=0x" + Integer.toHexString(off);
            }
            return "UNRECOGNIZED pattern";
        } catch (Exception e) {
            return "decode error: " + e.getMessage();
        }
    }

    private int leInt(byte[] b, int off) {
        return (b[off] & 0xff) | ((b[off+1] & 0xff) << 8) | ((b[off+2] & 0xff) << 16) | ((b[off+3] & 0xff) << 24);
    }
}
