// Decompile functions whose name (or namespace::name) matches a regex. args: <out file> <regex>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.regex.*;
public class DumpSome extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    String re = a[1].startsWith("@") ? new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(a[1].substring(1)))).trim() : a[1];
    Pattern p = Pattern.compile(re);
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    PrintWriter pw = new PrintWriter(new FileWriter(a[0]));
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      String n = f.getName(true);
      if (!p.matcher(n).find()) continue;
      DecompileResults r = di.decompileFunction(f, 120, monitor);
      pw.println("//==== " + f.getEntryPoint() + " " + n);
      pw.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed");
    }
    pw.close();
  }
}
