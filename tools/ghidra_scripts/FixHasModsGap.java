import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;

import java.io.FileWriter;
import java.io.PrintWriter;

public class FixHasModsGap extends GhidraScript {

    static final String OUT_PATH = "/home/itarasiuk/Dev/bg3-modding/bg3le/reference/ghidra_hasmods_gapfix.txt";
    static final long CALLEE_ADDR = 0x02554490L; // suspected operator new, maybe mis-flagged no-return
    static final long WINDOW_START = 0x04075e00L;
    static final long WINDOW_END = 0x04076300L;
    static final long REF_ADDR = 0x04075fa9L;

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(OUT_PATH));
        try {
            Function callee = getFunctionAt(toAddr(CALLEE_ADDR));
            if (callee == null) {
                out.println("No function at callee addr " + toAddr(CALLEE_ADDR) + "; trying to create one");
                DisassembleCommand dis0 = new DisassembleCommand(toAddr(CALLEE_ADDR), null, true);
                dis0.applyTo(currentProgram, monitor);
                CreateFunctionCmd cfc0 = new CreateFunctionCmd(toAddr(CALLEE_ADDR));
                cfc0.applyTo(currentProgram, monitor);
                callee = getFunctionAt(toAddr(CALLEE_ADDR));
            }
            if (callee != null) {
                out.println("callee " + callee.getName() + " hasNoReturn=" + callee.hasNoReturn());
                if (callee.hasNoReturn()) {
                    callee.setNoReturn(false);
                    out.println("cleared no-return; now=" + callee.hasNoReturn());
                }
            } else {
                out.println("STILL no callee function found");
            }

            // Force disassembly of the whole window
            DisassembleCommand dis = new DisassembleCommand(toAddr(WINDOW_START), null, true);
            dis.applyTo(currentProgram, monitor);

            // Repeatedly find gap starts (undefined bytes right after defined code) and
            // disassemble() from there directly, since DisassembleCommand alone doesn't
            // propagate through gaps left by a previously-mis-flagged no-return callee.
            for (int pass = 0; pass < 20; pass++) {
                Address cur2 = toAddr(WINDOW_START);
                Address end2 = toAddr(WINDOW_END);
                boolean anyFixed = false;
                while (cur2.compareTo(end2) <= 0) {
                    Instruction ins = getInstructionAt(cur2);
                    if (ins != null) {
                        cur2 = ins.getMaxAddress().add(1);
                        continue;
                    }
                    // gap start
                    disassemble(cur2);
                    Instruction after = getInstructionAt(cur2);
                    if (after != null) {
                        anyFixed = true;
                        cur2 = after.getMaxAddress().add(1);
                    } else {
                        cur2 = cur2.add(1);
                    }
                }
                if (!anyFixed) break;
            }

            // Try to (re)create the function containing REF_ADDR from its real start.
            // First clear any bogus function that starts exactly at REF_ADDR (created by a previous script).
            Function bogus = getFunctionAt(toAddr(REF_ADDR));
            if (bogus != null) {
                out.println("Removing bogus function at REF_ADDR: " + bogus.getName());
                currentProgram.getFunctionManager().removeFunction(toAddr(REF_ADDR));
            }

            out.println("--- raw instruction listing " + toAddr(WINDOW_START) + " .. " + toAddr(WINDOW_END) + " ---");
            Address cur = toAddr(WINDOW_START);
            Address end = toAddr(WINDOW_END);
            int count = 0;
            while (cur.compareTo(end) <= 0 && count < 6000) {
                Instruction ins = getInstructionAt(cur);
                if (ins != null) {
                    String marker = cur.equals(toAddr(REF_ADDR)) ? "  <== HasUnofficialMods ref" : "";
                    out.println(cur + ":  " + ins.toString() + marker);
                    cur = ins.getMaxAddress().add(1);
                } else {
                    byte b = 0;
                    try { b = getByte(cur); } catch (Exception e) {}
                    out.println(cur + ":  db 0x" + String.format("%02x", b & 0xff) + " (no instruction)");
                    cur = cur.add(1);
                }
                count++;
            }
        } finally {
            out.close();
        }
        println("Done, wrote " + OUT_PATH);
    }
}
