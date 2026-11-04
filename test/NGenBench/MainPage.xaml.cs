using System;
using System.Text;
using System.Windows.Controls;

namespace NGenBench
{
    public partial class MainPage : UserControl
    {
        private readonly StringBuilder results = new StringBuilder();
        private int passed, failed;

        public MainPage()
        {
            InitializeComponent();
            Check("CatchFirstMessage", Tests.CatchFirstMessage("abc") == "abc!" ? 1 : 0, 1);
            Check("Add", Tests.Add(40, 2), 42);
            Check("SumTo", Tests.SumTo(1000), 500500);
            Check("Mul64", Tests.Mul64(3000000000L, 3), 8999999999L);
            Check("Fib", Tests.Fib(15), 610);
            Check("Div", Tests.Div(-17, 5), -3);
            Check("Rem", Tests.Rem(-17, 5), -2);
            Check("DivUnsigned", (long)Tests.DivUnsigned(0xF0000000u, 16), 0x0F000000L);
            Check("Average", Tests.Average(3, 4), 35);
            Check("Narrow", Tests.Narrow(255), -7144); // (byte)256 + (short)255000
            Check("UnsignedLess", Tests.UnsignedLess(-1, 1) ? 1 : 0, 0);
            Check("Max", Tests.Max(-3, 7), 7);
            Check("Clamp", Tests.Clamp(12, 0, 10) * 100 + Tests.Clamp(-5, 0, 10), 1000);
            Check("Shifts", Tests.Shifts(-256, 4), -4096 + -16 + 0x0FFFFFFFFFFFFFF0L);
            Check("Greet", Tests.Greet("Xbox") == "Hello, Xbox" ? 1 : 0, 1);
            Check("CountChar", Tests.CountChar("native image", 'i'), 2);
            Check("Shapes", Tests.Shapes(), 9 + 1000);
            Check("Chain", Tests.Chain() == "43210" ? 1 : 0, 1);
            Check("Collatz", Tests.Collatz(27), 111);
            Check("Swap", Tests.Swap(1, 2), 21);
            Check("Garbage", Tests.Garbage(20000), 88889); // digits of 0..19999, less 1
            Check("Statics", Tests.Statics(), 45);
            Check("Label", Tests.Label() == "static45" ? 1 : 0, 1);
            Check("CrossModuleStatic", Tests.CrossModuleStatic(), 10);
            Check("WideStatics", Tests.WideStatics(), (1L << 40) + 5 + 3 + 100);
            Check("StructStatics", Tests.StructStatics(), 300 + 4 + 10);
            Check("NamedStructs", Tests.NamedStructs("abc"), 35 + 4500 + 40000 + 3 + 5000000);
            Check("TypeOf", Tests.TypeOf(), 500 + 70 + 1);
            Check("Constrained", Tests.Constrained("abc"), 500 + typeof(Vec).FullName.Length + 10000);
            Check("ArrayOffsets", Tests.ArrayOffsets() == "S4:0|11|21| S8:0,0|11,12|21,22| S8L:0,0|11,12|21,22| S12:0,0,0|11,12,13|21,22,23| S16:0,0,0,0|11,12,13,14|21,22,23,24| S16D:0,0|11,12|21,22| G16:0,0,0,0|11,12,13,14|21,22,23,24| S20:0,0,0,0,0|11,12,13,14,15|21,22,23,24,25| S24L:0,0,0|11,12,13|21,22,23| G12:0,0,0|11,12,13|21,22,23| G16R:0,0,0,0|11,12,3,14|21,22,3,24| S12R:0,0,0|1,12,3|1,22,3|" ? 1 : 0, 1);
            Check("ArrayClear", Tests.ArrayClearProbe() == "long 023 double 02.53.5 int 023 struct 023 string -bc s4 023 s8 023 s12 023" ? 1 : 0, 1);
            Check("ExceptionMessage", Tests.ExceptionMessage(), 111);
            Check("FloatStackArgs", Tests.FloatStackArgs(), 36 + 150 + 2250 + 50 + 125);
            Check("BigStruct", Tests.BigStruct(), (long)(14.5 + (1.5 + 4 + 3 + 10 + 100) * 100 + (2 + 4 + 1) * 10000));
            Check("GenericRetBuf", Tests.GenericRetBuf(), 13 + 29 * 100);
            Check("HugeStruct", Tests.HugeStruct(), (long)((3 + 3 + 6) + (5 + 5 + 100 + 7 + 10) * 100 + (3 + 3 + 6) * 10000 + 5 * 1000000));
            Check("LongMethod", LongTests.LongMethod(3), LongTests.LongMethodExpected(3));
            Check("TypedRef", Tests.TypedRef(), 11);
            Check("MdArrayAddress", Tests.MdArrayAddress(), 350007);
            Check("ThreadStaticCctor", Tests.ThreadStaticCctor(), 100 * 10000 + 1 + 50 + 400 + 7000);
            Check("LeafRegisters", Tests.LeafRegisters(), (-56 + 44 - 5536 + 66) * 1000L + (1 - 4 + 9 - 16 + 5000000000L));
            Check("SavedRegistersGC", Tests.SavedRegistersGC(), 2456);
            Check("UnwindSaved", Tests.UnwindSaved(0), 4 * (3000001234L + 1));
            Check("BarrierRegisters", Tests.BarrierRegisters(20), 860 + 50); // sum(4i + 5) + the strings' lengths
            Check("PInvokeUnderGC", Tests.PInvokeUnderGC(2000), 2000 * (1 + 8 + 7 + 1));
            Check("FusedStores", Tests.FusedStores(50), 3263590180054050951L);
            Check("SavedAcrossHandlers", Tests.SavedAcrossHandlers(2), 51539908317L);
            Check("SavedFloats", Tests.SavedFloats(4), -188998); // (-270 - 135 + 4 * (8 + 34 + 12)) * 1000 + 2
            Check("AliasedFloats", Tests.AliasedFloats(4), 994700012345679104L); // 9947 * 10^14 + (long)((float)(1/81) * 10^12)
            Check("BarrierBelow", Tests.BarrierBelow(20), 199480666214821982L);
            Check("LeafFloats", Tests.LeafFloats(), 8880); // (5.25 + 8.25 + 3.25 + 2.75) * 100 + 7 * 1000 - 7 * 10
            Check("LeafStructs", Tests.LeafStructs(), 4879659989999301L); // 1 - 100007 * 100 - 12034 * 10^10 + 5 * 10^15
            Check("OneFieldStructs", Tests.OneFieldStructs(), 21 + 4000 + 25000);
            Check("StelemRefPaths", Tests.StelemRefPaths(), 23 * 10000 + 100 + 1000 + 1 + 10 + 1 + 20 + 5);
            Check("LeaveCatchFinally", Tests.LeaveCatchFinally(), (10 * 100000L + 10110) * 100 - 1 + 11110);
            Check("GenericVirtual", Tests.GenericVirtual(), 13 + (11 + 1200) * 10 + (10 + 1100) * 10000);
            Check("GenericLdftn", Tests.GenericLdftn(), 4 + 50 + 300);
            Check("EnumNames", (Color.Green.ToString() + Enum.GetName(typeof(Color), 2) + Color.Red) == "GreenBlueRed" ? 1 : 0, 1);
            Check("StackArgs", Tests.StackArgs(), (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 900 + 10000) + 1100);
            Check("Arrays", Tests.Arrays(), 285 + 10);
            Check("RefArrays", Tests.RefArrays() == "abc" ? 1 : 0, 1);
            Check("Bytes", Tests.Bytes(), 200 + 100 + 4 + 65);
            Check("Boxing", Tests.Boxing(), 4211);
            Check("Switch", Tests.Switch(0) + Tests.Switch(2) * 10 + Tests.Switch(7) * 100, 10 + 300 - 100);
            Check("Generic", Tests.Generic(), 10);
            Node counter = new Node(1, null);
            int first = Tests.TryFinally(counter, 2), second = Tests.TryFinally(counter, 7);
            Check("TryFinally", first + second + counter.Value, 30 - 1 + 370);
            int[] numbers = { 10, 20, 30 };
            Check("TryCatch", Tests.TryCatch(numbers, 1) * 100 + Tests.TryCatch(numbers, 5) * 10 + Tests.TryCatch(null, 0),
                2000 - 10 - 2);
            Check("ThrowCatch", Tests.ThrowCatch(-1) == "negative" && Tests.ThrowCatch(1) == "ok" ? 1 : 0, 1);
            Check("Nested", Tests.Nested(new Node(0, null)), 123);
            Node log = new Node(0, null);
            Check("CatchThroughFinally", Tests.CatchThroughFinally(log) + log.Value, 1000 + 101);
            log = new Node(0, null);
            Check("TwoFinallies", Tests.TwoFinallies(log) * 1000 + log.Value, 1123);
            Check("Rethrow", Tests.Rethrow(new Node(0, null)), 11);
            Check("GcInHandlers", Tests.GcInHandlers(20000), 88889 + 7 + 1 + 12);
            Check("Interfaces", Tests.Interfaces(), 3 + 300);
            Check("Using", Tests.Using(new Counter()), 11);
            Check("Foreach", Tests.Foreach(5), 0 + 1 + 4 + 9 + 16);
            Check("FloatArgs", (long)(Tests.FloatArgs(1.5, 3, 2.5, 0.5f) * 10), 75);
            Check("CallFloatArgs", (long)(Tests.CallFloatArgs() * 10), 75);
            Check("VecMath", (long)Tests.VecMath(), 52 + 8);
            Check("SmallStructs", Tests.SmallStructs(), 42231);
            Check("StructFields", (long)Tests.StructFields(), 400 + 4 + 10);
            Check("PointRect", (long)Tests.PointRect(), 100 + 4 + 6);
            Check("BoxStruct", (long)Tests.BoxStruct(), 6);
            Vec scaled = Tests.Scale(new Vec(1.5, 2.5), 2);
            Check("ScaleFromInterpreter", (long)(scaled.X * 10 + scaled.Y), 30 + 5);
            Check("VirtualDelegate", Tests.Delegates(0), 15);
            Check("InstanceDelegate", Tests.Delegates(1), 50);
            Check("Closure", Tests.Delegates(2), 13);
            Check("StaticDelegate", Tests.Delegates(3), 20);
            Check("Multicast", Tests.Multicast(new Counter()), 200 + 200);
            Check("ArrayInit", Tests.ArrayInit(), 41 * 10 + 5);
            Check("IntPtrCtor", Tests.IntPtrCtor(21), 21 + 42);
            Check("SharedCalls", Tests.SharedCalls("xyz"), 321 + 1430);
            Check("SharedLookups", Tests.SharedLookupCalls("xyz"), (3 + 10 + 600 + 1000) + (3 + 600));
            Check("TryInHandler0", Tests.TryInHandler(0), 1);
            Check("TryInHandler1", Tests.TryInHandler(1), 110110);
            Check("TryInHandler2", Tests.TryInHandler(2), 111010);
            Check("SharedNew", Tests.SharedNewCalls("xyz"), 1 + 10);
            Check("BigStruct", Tests.BigStruct("abcd"), (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9) + 4000);
            Check("StackAlloc", Tests.StackAlloc(100), 4950 + 1000 + 198);
            Check("StackAllocArgs", Tests.StackAllocArgs(10), 36 + 400 + 1000 + 9);
            Check("Checked", Tests.Checked(100, 100000), 300 + 100005 + 200 + 100 + 1000 + 10000 + 100000 + 100 + 100000 - 1);
            Check("LongArrays", Tests.LongArrays(10), 45 + 22);
            Check("SharedStatics", Tests.SharedStatics("a", "bb"), 300 + 20 + 3);
            Check("Strings", Tests.Strings(5), 50 + 2 + 100);
            Check("KvpBig", Tests.KvpBig("abc"), 7);
            Check("GenericIface", Tests.GenericIface(), 3 + 50);
            Check("DictStructRef", Tests.DictStructRef(10), 45 + 20);
            Check("EntryArray", Tests.EntryArray(), 406258);
            Check("DictVariants", Tests.DictVariants(), 63);
            Check("ResizeProbe", Tests.ResizeProbe(), 7);
            Check("MyList", Tests.MyListTest(20), 50 * 100 + 32);
            Check("NullableUnbox", Tests.NullableUnbox(7), 7 + 70 + 100);
            Check("EventReflection", Tests.EventReflection(), 3 * (20 + 100));
            Check("EventReflectionGc", Tests.EventReflectionGc(), 12 * (40 + 100));
            Check("FloatRemConv", Tests.FloatRemConv(10.5), 122602615);
            Check("MultiDim", Tests.MultiDim(3), 3 * (0 + 1 + 2 + 3) + 10 * (0 + 1 + 2) * 4 + 12 * 1000 + 8 * 100000 + 1000000 + 20000000);
            Check("StructCopy", Tests.StructCopy(), 321);
            Check("ArrayCopyProbe", Tests.ArrayCopyProbe(), 12340000 + 8191);
            Check("DictAfterResize", Tests.DictAfterResize(), 414);
            Check("GenericEntryArray", Tests.GenericEntryArray(), 406258);
            Check("SmallStructs", Tests.SmallStructs(5), 102 + 100 + 30 + 30000); // Int16.CompareTo returns the difference
            Check("GenericDelegate", Tests.GenericDelegate("abc"), 30);
            Check("GenericCalls", Tests.GenericCalls("abc"), 1442);
            StackPanel tree = new StackPanel(), inner = new StackPanel();
            tree.Children.Add(new Button { Content = "a" });
            tree.Children.Add(inner);
            inner.Children.Add(new Button { Content = "b" });
            inner.Children.Add(new Button { Content = "c" });
            tree.Children.Add(new Button { Content = "d" });
            string leaves = Tests.LeafNames(tree);
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: leaves '" + leaves + "'");
            Check("Leaves", leaves == "abcd" ? 1 : 0, 1);
            try
            {
                TextBlock status = new TextBlock();
                Button tile = new Button();
                tile.Content = "tile";
                Tests.ShowContent(status, tile);
                Check("ShowContent", status.Text == "Focused: tile" ? 1 : 0, 1);
            }
            catch (Exception ex)
            {
                XboxSdk.Samples.DebugLog.WriteLine("NGenBench: ShowContent threw " + ex);
                Check("ShowContent", 0, 1);
            }
            string timings = RunTimings();
            LogProbes();
            SummaryText.Text = passed + " passed, " + failed + " failed";
            ResultsText.Text = results.ToString();

            int start = Environment.TickCount;
            int sum = Tests.SumTo(3000000);
            int sumMs = Environment.TickCount - start;
            start = Environment.TickCount;
            int fib = Tests.Fib(24);
            int fibMs = Environment.TickCount - start;
            start = Environment.TickCount;
            int collatz = 0;
            for (int i = 1; i < 3000; i++)
                collatz += Tests.Collatz(i);
            int collatzMs = Environment.TickCount - start;
            TimingText.Text = "SumTo(3000000): " + sumMs + " ms   Fib(24): " + fibMs + " ms   Collatz(1..3000): " + collatzMs + " ms\n" + timings;
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: " + SummaryText.Text + "; " + TimingText.Text + " (" + sum + "," + fib + "," + collatz + ")");
        }

        // The timing loops (Timing.cs): each logged as it ends ("NGenBench: time <name> <ms> ms", which Bench-Timing.ps1
        // reads, and keeps the log from going quiet on an interpreted run) and its checksum checked. Interpreted (this
        // assembly without its image: SumTo(1000000) takes 1 ms compiled, about 90 interpreted) the loops that run
        // the bench's own code do a sixteenth of the work and say so ("/16"): in full they outlast Run-App's patience.
        private delegate int TimingLoop();

        private string RunTimings()
        {
            int start = Environment.TickCount;
            Tests.SumTo(1000000);
            int part = Environment.TickCount - start > 20 ? 16 : 1;
            StringBuilder line = new StringBuilder();
            Time(line, "StringBuild", 1, () => Timing.StringBuild(300000), 3188890);
            Time(line, "DictionaryLookups", 1, () => Timing.DictionaryLookups(20), 335861660);
            if (part == 1)
            {
                Time(line, "ListIterate", 1, () => Timing.ListIterate(80), 1558554624);
                Time(line, "FieldObjects", 1, () => Timing.FieldObjects(8000), 226160);
                Time(line, "Mandelbrot", 1, () => Timing.Mandelbrot(640, 480), 14481762);
                Time(line, "MathCalls", 1, () => Timing.MathCalls(4000000), 1333334016);
            }
            else
            {
                Time(line, "ListIterate", part, () => Timing.ListIterate(5), 259086976);
                Time(line, "FieldObjects", part, () => Timing.FieldObjects(500), 14100);
                Time(line, "Mandelbrot", part, () => Timing.Mandelbrot(160, 120), 909018);
                Time(line, "MathCalls", part, () => Timing.MathCalls(250000), 20833585);
            }
            return line.ToString();
        }

        private void Time(StringBuilder line, string name, int part, TimingLoop loop, int expected)
        {
            int start = Environment.TickCount;
            int result = loop();
            int ms = Environment.TickCount - start;
            string scale = part > 1 ? " /" + part : "";
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: time " + name + " " + ms + " ms" + scale);
            line.Append(name).Append(": ").Append(ms).Append(" ms").Append(scale).Append("   ");
            Check(name, result, expected);
        }

        private static void LogProbes()
        {
            Pair returned = ProbeReturns.XNGenW(7);
            System.Windows.Point returnedPoint = ProbeReturns.XNGenV(7);
            System.Windows.Point instancePoint = new ProbeInstance().XNGenI(7);
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe instance return " + instancePoint.X + "," + instancePoint.Y);
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe stack from compiled code 90.." + Tests.StackProbe());
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe stack 18.." +
                ProbeStack.XNGenS18(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS1C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS20(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS24(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS28(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS2C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS30(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS34(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS38(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS3C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS40(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS44(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS48(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS4C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS50(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS54(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS58(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS5C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS60(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS64(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS68(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS6C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS70(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS74(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS78(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS7C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS80(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS84(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS88(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS8C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x"));
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe stack 90.." +
                ProbeStack.XNGenS90(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS94(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS98(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS9C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenSA0(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenSA4(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenSA8(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenSAC(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x"));
            Small small = new Small { V = 0x5555 };
            Twelve twelve = new Twelve { A = 0x1111, B = 0x2222, C = 0x3333 };
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe small r3 " + ProbeSmall.XNGenR3(small, 7).ToString("x") + " r4 " +
                ProbeSmall.XNGenR4(small, 7).ToString("x") + " returned " + ProbeSmall.XNGenW(7).V.ToString("x") + " twelve r3..r5 " +
                ProbeTwelve.XNGenR3(twelve, 7).ToString("x") + " " + ProbeTwelve.XNGenR4(twelve, 7).ToString("x") + " " +
                ProbeTwelve.XNGenR5(twelve, 7).ToString("x"));
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe returns pair " + returned.A.ToString("x") + ":" + returned.B.ToString("x") +
                " point " + returnedPoint.X + "," + returnedPoint.Y);
            Pair pair = new Pair { A = 0x1111, B = 0x2222 };
            System.Windows.Point point = new System.Windows.Point(1.5, 2.5);
            System.Windows.Rect rect = new System.Windows.Rect(3, 4, 5, 6);
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe doubles r3..r7 " + ProbeDoubles.XNGenR3(1.5, 7, 2.5, 9).ToString("x") + " " +
                ProbeDoubles.XNGenR4(1.5, 7, 2.5, 9).ToString("x") + " " + ProbeDoubles.XNGenR5(1.5, 7, 2.5, 9).ToString("x") + " " +
                ProbeDoubles.XNGenR6(1.5, 7, 2.5, 9).ToString("x") + " " + ProbeDoubles.XNGenR7(1.5, 7, 2.5, 9).ToString("x") + " f1..f3 " +
                ProbeDoubles.XNGenF1(1.5, 7, 2.5, 9) + " " + ProbeDoubles.XNGenF2(1.5, 7, 2.5, 9) + " " + ProbeDoubles.XNGenF3(1.5, 7, 2.5, 9));
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe point r3..r7 " + ProbePoint.XNGenR3(point, 7).ToString("x") + " " +
                ProbePoint.XNGenR4(point, 7).ToString("x") + " " + ProbePoint.XNGenR5(point, 7).ToString("x") + " " +
                ProbePoint.XNGenR6(point, 7).ToString("x") + " " + ProbePoint.XNGenR7(point, 7).ToString("x") + " f1..f4 " +
                ProbePoint.XNGenF1(point, 7) + " " + ProbePoint.XNGenF2(point, 7) + " " + ProbePoint.XNGenF3(point, 7) + " " + ProbePoint.XNGenF4(point, 7));
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe pair r3..r6 " + ProbePair.XNGenR3(pair, 7).ToString("x") + " " +
                ProbePair.XNGenR4(pair, 7).ToString("x") + " " + ProbePair.XNGenR5(pair, 7).ToString("x") + " " +
                ProbePair.XNGenR6(pair, 7).ToString("x") + " f1 " + ProbePair.XNGenF1(pair, 7));
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: probe rect r3..r8 " + ProbeRect.XNGenR3(5, rect, 7).ToString("x") + " " +
                ProbeRect.XNGenR4(5, rect, 7).ToString("x") + " " + ProbeRect.XNGenR5(5, rect, 7).ToString("x") + " " +
                ProbeRect.XNGenR6(5, rect, 7).ToString("x") + " " + ProbeRect.XNGenR7(5, rect, 7).ToString("x") + " " +
                ProbeRect.XNGenR8(5, rect, 7).ToString("x") + " f1..f5 " + ProbeRect.XNGenF1(5, rect, 7) + " " + ProbeRect.XNGenF2(5, rect, 7) + " " +
                ProbeRect.XNGenF3(5, rect, 7) + " " + ProbeRect.XNGenF4(5, rect, 7) + " " + ProbeRect.XNGenF5(5, rect, 7));
        }

        private void Check(string name, long actual, long expected)
        {
            bool ok = actual == expected;
            if (ok) passed++; else failed++;
            results.Append(name).Append(ok ? " ok   " : " FAIL(" + actual + " != " + expected + ")   ");
            XboxSdk.Samples.DebugLog.WriteLine("NGenBench: " + name + (ok ? " ok" : " FAIL " + actual + " expected " + expected));
        }
    }
}
