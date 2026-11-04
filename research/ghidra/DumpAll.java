// Decompile every function to <out>.c and list functions to <out>.funcs
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpAll extends GhidraScript {
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    PrintWriter pw = new PrintWriter(new FileWriter(out + ".c"));
    PrintWriter fl = new PrintWriter(new FileWriter(out + ".funcs"));
    FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
    int n = 0;
    while (it.hasNext() && !monitor.isCancelled()) {
      Function f = it.next();
      fl.println(f.getEntryPoint() + " " + f.getName() + " " + f.getBody().getNumAddresses());
      DecompileResults r = di.decompileFunction(f, 60, monitor);
      pw.println("//==== " + f.getEntryPoint() + " " + f.getName());
      if (r != null && r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
      else pw.println("// decompile failed");
      if (++n % 500 == 0) { pw.flush(); println("done " + n); }
    }
    pw.close(); fl.close();
  }
}
