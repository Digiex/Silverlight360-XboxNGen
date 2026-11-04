// Create functions at every .pdata entry (Xbox 360 PPC RUNTIME_FUNCTION: begin, packed len)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
public class PdataFuncs extends GhidraScript {
  public void run() throws Exception {
    String[] sa = getScriptArgs(); Address a = toAddr(Long.parseLong(sa[0],16)); long n = Long.parseLong(sa[1],16) / 8; int made = 0;
    java.util.List<Address> ents = new java.util.ArrayList<>();
    for (long i = 0; i < n; i++) {
      int begin = getInt(a.add(i * 8)); int w = getInt(a.add(i * 8 + 4));
      if (i < 3) println("pd " + a.add(i*8) + " begin " + Integer.toHexString(begin) + " blk " + currentProgram.getMemory().getBlock(toAddr(begin & 0xffffffffL)));
      if (begin == 0) continue;
      Address fa = toAddr(begin & 0xffffffffL);
      if (currentProgram.getMemory().getBlock(fa) == null) continue;
      long len = ((w >>> 8) & 0x3fffff) * 4L;
      if (getInstructionAt(fa) == null)
        new DisassembleCommand(new AddressSet(fa, fa.add(Math.max(len, 4) - 1)), null, true).applyTo(currentProgram, monitor);
      ents.add(fa);
    }
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      if (!f.isThunk() && !ents.contains(f.getEntryPoint())) continue;
    }
    java.util.Set<Address> es = new java.util.HashSet<>(ents);
    java.util.List<Function> kill = new java.util.ArrayList<>();
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) kill.add(f);
    if (ents.size() < 100) { println("too few entries, abort"); return; }
    for (Function f : kill) removeFunction(f);
    java.util.Collections.reverse(ents);
    println("entries: " + ents.size() + " first " + (ents.isEmpty()?"-":ents.get(ents.size()-1).toString()));
    int fail=0;
    for (Address fa : ents) {
      if (getInstructionAt(fa) == null) { disassemble(fa); }
      Function f = createFunction(fa, null);
      if (f != null) made++; else if (fail++ < 5) println("fail at " + fa + " insn=" + getInstructionAt(fa));
    }
    println("pdata functions created: " + made);
    analyzeChanges(currentProgram);
  }
}
