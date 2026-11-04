using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace NGenBench
{
    // Methods for xclrjit to compile, each exercising one area. MainPage checks their results against values worked
    // out by hand, so a wrong compilation shows up as a failure whether the method ran natively or was interpreted.
    public static class Tests
    {
        public static int Add(int a, int b) { return a + b; }

        public static int SumTo(int n)
        {
            int sum = 0;
            for (int i = 1; i <= n; i++)
                sum += i;
            return sum;
        }

        public static long Mul64(long a, long b) { return a * b - 1; }

        public static int Fib(int n) { return n < 2 ? n : Fib(n - 1) + Fib(n - 2); }

        public static int Div(int a, int b) { return a / b; }

        public static int Rem(int a, int b) { return a % b; }

        public static uint DivUnsigned(uint a, uint b) { return a / b; }

        public static int Average(int a, int b)
        {
            double d = (a + b) / 2.0;
            return (int)(d * 10.0);
        }

        public static int Narrow(int x)
        {
            byte b = (byte)(x + 1);
            short s = (short)(x * 1000);
            return b + s;
        }

        public static bool UnsignedLess(int a, int b) { return (uint)a < (uint)b; }

        public static int Max(int a, int b) { return a > b ? a : b; }

        public static int Clamp(int x, int lo, int hi)
        {
            if (x < lo)
                return lo;
            if (x > hi)
                return hi;
            return x;
        }

        public static long Shifts(long x, int n) { return (x << n) + (x >> n) + (long)((ulong)x >> n); }

        public static string Greet(string name) { return "Hello, " + name; }

        public static int CountChar(string s, char ch)
        {
            int count = 0;
            for (int i = 0; i < s.Length; i++)
                if (s[i] == ch)
                    count++;
            return count;
        }

        public static int Shapes()
        {
            Shape a = new Square(3);
            Shape b = new Rectangle(2, 5);
            return a.Area() + b.Area() * 100;
        }

        public static string Chain()
        {
            Node head = null;
            for (int i = 0; i < 5; i++)
                head = new Node(i, head);
            string s = "";
            for (Node n = head; n != null; n = n.Next)
                s = s + n.Value;
            return s;
        }

        public static int Collatz(int n)
        {
            int steps = 0;
            long x = n;
            while (x != 1)
            {
                x = (x & 1) == 0 ? x / 2 : 3 * x + 1;
                steps++;
            }
            return steps;
        }

        public static int Swap(int a, int b)
        {
            Exchange(ref a, ref b);
            return a * 10 + b;
        }

        public static void Exchange(ref int x, ref int y)
        {
            int t = x;
            x = y;
            y = t;
        }

        private static int counter;
        private static string label = "static";

        public static int Statics()
        {
            counter = 0;
            for (int i = 0; i < 10; i++)
                counter += i;
            label = label + counter;
            return counter;
        }

        public static string Label() { return label; }

        public static int Arrays()
        {
            int[] a = new int[10];
            for (int i = 0; i < a.Length; i++)
                a[i] = i * i;
            int sum = 0;
            foreach (int x in a)
                sum += x;
            return sum + a.Length;
        }

        public static string RefArrays()
        {
            string[] s = new string[3];
            s[0] = "a";
            s[1] = "b";
            s[2] = "c";
            return s[0] + s[1] + s[2];
        }

        public static int Bytes()
        {
            byte[] b = new byte[4];
            b[0] = 200;
            b[1] = 100;
            char[] c = new char[2];
            c[1] = 'A';
            return b[0] + b[1] + b.Length + c[1];
        }

        public static int Boxing()
        {
            object o = 42;
            object p = "x";
            int r = (int)o;
            return (o is int ? 1 : 0) + (p is string ? 10 : 0) + r * 100;
        }

        public static int Switch(int x)
        {
            switch (x)
            {
                case 0: return 10;
                case 1: return 20;
                case 2: return 30;
                default: return -1;
            }
        }

        public static int Generic()
        {
            System.Collections.Generic.List<int> list = new System.Collections.Generic.List<int>();
            for (int i = 0; i < 5; i++)
                list.Add(i);
            int sum = 0;
            for (int i = 0; i < list.Count; i++)
                sum += list[i];
            return sum;
        }

        public static int Garbage(int rounds)
        {
            // Allocates enough to make the GC run while compiled frames hold references.
            Node keep = new Node(-1, null);
            int total = 0;
            for (int i = 0; i < rounds; i++)
            {
                Node n = new Node(i, keep);
                string s = "x" + i;
                total += s.Length + n.Next.Value;
            }
            return total + keep.Value;
        }

        // Exception handling: finally, catch, nested handlers, leaves that run finallys, rethrow.
        public static int TryFinally(Node counter, int x)
        {
            try
            {
                counter.Value += x;
                if (x > 5)
                    return -1;
            }
            finally
            {
                counter.Value *= 10;
            }
            return counter.Value;
        }

        public static int TryCatch(int[] a, int i)
        {
            try
            {
                return a[i];
            }
            catch (IndexOutOfRangeException)
            {
                return -1;
            }
            catch (NullReferenceException)
            {
                return -2;
            }
        }

        public static string ThrowCatch(int x)
        {
            try
            {
                if (x < 0)
                    throw new ArgumentException("negative");
                return "ok";
            }
            catch (ArgumentException e)
            {
                return e.Message;
            }
        }

        public static void Fail(Node log)
        {
            throw new InvalidOperationException("fail " + log.Value);
        }

        public static int Nested(Node log)
        {
            try
            {
                try
                {
                    log.Value = log.Value * 10 + 1;
                    Fail(log);
                    log.Value = 0;
                }
                finally
                {
                    log.Value = log.Value * 10 + 2;
                }
            }
            catch (InvalidOperationException)
            {
                log.Value = log.Value * 10 + 3;
            }
            return log.Value;
        }

        public static int CatchThroughFinally(Node log)
        {
            try
            {
                try
                {
                    Fail(log);
                    return 0;
                }
                catch (InvalidOperationException)
                {
                    log.Value += 1;
                    return log.Value * 1000;
                }
            }
            finally
            {
                log.Value += 100;
            }
        }

        public static int TwoFinallies(Node log)
        {
            try
            {
                try
                {
                    log.Value = 1;
                    return log.Value;
                }
                finally
                {
                    log.Value = log.Value * 10 + 2;
                }
            }
            finally
            {
                log.Value = log.Value * 10 + 3;
            }
        }

        public static int Rethrow(Node log)
        {
            try
            {
                try
                {
                    Fail(log);
                }
                catch (InvalidOperationException)
                {
                    log.Value += 1;
                    throw;
                }
            }
            catch (InvalidOperationException)
            {
                log.Value += 10;
            }
            return log.Value;
        }

        public static int GcInHandlers(int rounds)
        {
            // The GC runs inside a catch and a finally while the parent frame holds the only references.
            Node keep = new Node(7, null);
            Node other = new Node(5, keep);
            int r = 0;
            try
            {
                Fail(keep);
            }
            catch (InvalidOperationException)
            {
                r += Garbage(rounds);
                r += other.Next.Value;
            }
            try
            {
                r += 1;
            }
            finally
            {
                r += Garbage(rounds) - Garbage(rounds);
                r += keep.Value + other.Value;
            }
            return r;
        }

        // What XboxControls' GotFocus handler does: a ContentControl's content into a TextBlock.
        public static void ShowContent(TextBlock status, ContentControl tile)
        {
            status.Text = "Focused: " + tile.Content;
        }

        // PageNavigation's DirectionalFocus.Focusable: a recursive iterator over the visual tree.
        private static IEnumerable<Control> Leaves(DependencyObject parent)
        {
            int count = VisualTreeHelper.GetChildrenCount(parent);
            for (int i = 0; i < count; i++)
            {
                DependencyObject child = VisualTreeHelper.GetChild(parent, i);
                Control control = child as Control;
                if (control != null)
                {
                    yield return control;
                    continue;
                }
                foreach (Control nested in Leaves(child))
                    yield return nested;
            }
        }

        public static string LeafNames(DependencyObject root)
        {
            string names = "";
            foreach (Control c in Leaves(root))
                names += ((ContentControl)c).Content;
            return names;
        }

        // Delegates: a static method, a virtual and a non-virtual instance method, a closure.
        public static int Twice(int x) { return x * 2; }

        public static int Delegates(int which)
        {
            Adder adder = new Adder(5);
            int k = 3;
            IntOp op;
            if (which == 0)
                op = adder.Apply;
            else if (which == 1)
                op = adder.Times;
            else if (which == 2)
                op = x => x + k;
            else
                op = Twice;
            return op(10);
        }

        public static int Multicast(Counter counter)
        {
            IntOp both = x => counter.Next();
            both += x => counter.Next() * 100;
            return both(0) + counter.Total;
        }

        // A generic delegate over a reference type: Combine's result is cast back to the exact type.
        public static int GenericDelegate(string text)
        {
            Map<string, int> length = s => s.Length;
            length += s => s.Length * 10;
            return length(text);
        }

        // Calls of generic method instantiations: over a value type (its own code) and over reference types (an
        // instantiating stub into shared code), static and instance, and a generic method of mscorlib's.
        public static T Pick<T>(T a, T b, bool first) { return first ? a : b; }

        public static int GenericCalls(string text)
        {
            int n = Pick(3, 4, false) * 10;                       // 40
            n += Pick("ab", text, true).Length;                   // 42
            n += new Adder(1).Last(text, "wxyz").Length * 100;    // 442
            var list = new System.Collections.Generic.List<string>();
            list.Add(text);
            n += Pick(list, null, true).Count * 1000;             // 1442
            return n;
        }

        // Statics in another module (mscorlib's String.Empty), and wide ones in a class with a static constructor.
        public static int CrossModuleStatic()
        {
            return string.Empty != null ? 10 + string.Empty.Length : 0;
        }

        // Statics of value types: boxed in the GC statics (TimeSpan, a user struct), read, written and addressed.
        public static long StructStatics()
        {
            Holder.Elapsed += TimeSpan.FromSeconds(2);
            Holder.Origin.X += 3;
            Vec v = Holder.Origin;
            v.Y = 10;
            Holder.Origin = v;
            return Holder.Elapsed.Ticks / TimeSpan.TicksPerSecond * 100 + (long)(Holder.Origin.X + Holder.Origin.Y);
        }

        // Structs with references: locals, arguments, results, fields, an array, boxing, and a GC in between.
        public static int NamedStructs(string text)
        {
            Named a = new Named(text, 5);
            Named b = Rename(a, text + "!");
            b.Tag = new int[3];
            Named[] array = new Named[2];
            array[1] = b;
            GC.Collect();
            object boxed = a;
            Named c = (Named)boxed;
            Holder.Last = c;
            return a.Weight + b.Weight * 100 + array[1].Name.Length * 10000 + ((int[])array[1].Tag).Length
                + Holder.Last.Id * 1000000;
        }

        private static Named Rename(Named n, string name)
        {
            n.Name = name;
            return n;
        }

        // typeof: ldtoken and Type.GetTypeFromHandle.
        public static int TypeOf()
        {
            return typeof(Named).Name.Length * 100 + typeof(int[]).Name.Length * 10 + (typeof(Tests).IsClass ? 1 : 0);
        }

        // constrained. callvirt: an enum's and a struct's Object methods (boxed), a class through a pointer.
        public static int Constrained(string text)
        {
            Color color = Color.Green;
            Vec v = new Vec(1, 2);
            return color.ToString().Length * 100 + v.ToString().Length + (text.GetHashCode() == "abc".GetHashCode() ? 10000 : 0);
        }

        // Calls the stack probes from compiled code (the interpreter's call frames differ).
        public static string StackProbe()
        {
            return ProbeStack.XNGenS50(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS54(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS58(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x") + " " +
                ProbeStack.XNGenS5C(1, 2, 3, 4, 5, 6, 7, 8, 0x99, 0xAA).ToString("x");
        }

        // Ten arguments of mixed kinds, caller and callee both compiled: the last two go on the stack.
        public static long Sum10(int a, long b, int c, int d, string e, int f, int g, double h, int i, long j)
        {
            return a + b + c + d + e.Length + f + g + (long)h + i * 100 + j * 1000;
        }

        public static long StackArgs()
        {
            return Sum10(1, 2L, 3, 4, "abcde", 6, 7, 8.5, 9, 10L) + Sum10(0, 0, 0, 0, "", 0, 0, 0, 1, 1);
        }

        public static long WideStatics()
        {
            Holder.Big += 5;
            Holder.Ratio *= 2;
            return Holder.Big + (long)Holder.Ratio + Holder.Initialized;
        }

        // An array initializer: ldtoken of a field and RuntimeHelpers.InitializeArray.
        static readonly int[] Primes = { 2, 3, 5, 7, 11, 13 };
        static readonly char[] Vowels = { 'a', 'e', 'i', 'o', 'u' };

        public static int ArrayInit()
        {
            int n = 0;
            for (int i = 0; i < Primes.Length; i++)
                n += Primes[i];
            return n * 10 + Vowels.Length;
        }

        // newobj of a value type the runtime reports as a primitive (IntPtr), and its explicit conversion back.
        public static int IntPtrCtor(int x)
        {
            IntPtr p = new IntPtr(x);
            return (int)p + (int)(IntPtr)(x * 2);
        }

        // Shared generic code (T a reference type: one body for every reference instantiation) takes a hidden
        // instantiation argument before its own: these check the arguments still arrive in the right registers.
        public static int SharedArgs<T>(T a, string b, int c, T d) where T : class
        {
            return (a == null ? 0 : 1) + b.Length * 10 + c * 100 + (d == null ? 0 : 1000);
        }

        public static int SharedCalls(string text)
        {
            int n = SharedArgs<string>(text, "ab", 3, null);         // 1 + 20 + 300
            n += SharedArgs<object>(null, "abc", 4, new object());  // 0 + 30 + 400 + 1000
            return n;
        }

        // Shared generic code that needs the exact type at run time: the runtime's lookups through the hidden
        // instantiation argument (a static generic method) and through this (a generic class's method).
        public static int SharedLookups<T>(object o, T item) where T : class
        {
            T[] array = new T[3];                       // newarr T
            array[0] = item;
            int n = array.Length;                       // 3
            if (o is T) n += 10;                        // isinst T
            n += typeof(T).Name.Length * 100;           // ldtoken T
            n += ((T)o == null ? 0 : 1) * 1000;         // castclass T
            return n;
        }

        public static int SharedLookupCalls(string text)
        {
            return SharedLookups<string>(text, text) + SharedLookups<object>(null, text);   // (3+10+600+1000) + (3+600)
        }

        // A try inside a catch handler (a funclet with its own handlers): catch and finally in a catch.
        public static int TryInHandler(int x)
        {
            int n = 0;
            try
            {
                if (x > 0) throw new InvalidOperationException();
                n += 1;
            }
            catch (InvalidOperationException)
            {
                try
                {
                    n += 10;
                    if (x > 1) throw new ArgumentException();
                    n += 100;
                }
                catch (ArgumentException) { n += 1000; }
                finally { n += 10000; }
                n += 100000;
            }
            return n;
        }

        // newobj of a generic class from shared code: the exact class through a runtime lookup.
        public static int SharedNew<T>(T item) where T : class
        {
            var list = new List<T>();
            list.Add(item);
            return list.Count;
        }

        public static int SharedNewCalls(string text)
        {
            return SharedNew<string>(text) + SharedNew<object>(null) * 10;
        }

        // A value type of 80 bytes (over 16 words) holding a reference in its last word.
        // localloc: a stack block per round, used across a call that allocates, and a throw through the frame.
        public static unsafe long StackAlloc(int n)
        {
            long sum = 0;
            for (int round = 0; round < 3; round++)
            {
                int* p = stackalloc int[n];
                for (int i = 0; i < n; i++) p[i] = i * round;
                Garbage(200);
                try
                {
                    if (round == 2) throw new InvalidOperationException("round");
                    for (int i = 0; i < n; i++) sum += p[i];
                }
                catch (InvalidOperationException)
                {
                    sum += 1000 + p[n - 1];
                }
            }
            return sum;
        }

        // A call with arguments on the stack (the ninth and tenth) from a method that has done a localloc: the
        // outgoing area is at the moved stack pointer, not at the frame register (PopulateEvents passed a List this
        // way and List.Add got stack words).
        public static unsafe long StackAllocArgs(int n)
        {
            int* p = stackalloc int[n];
            for (int i = 0; i < n; i++) p[i] = i;
            var list = new List<int>();
            long r = TenArgs(p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], list, "nine");
            return r + list.Count * 1000 + p[n - 1];
        }

        private static long TenArgs(int a, int b, int c, int d, int e, int f, int g, int h, List<int> list, string s)
        {
            list.Add(a + b + c + d + e + f + g + h);
            return list[0] + s.Length * 100;
        }

        // Checked arithmetic and conversions, 32- and 64-bit, signed and unsigned, with the overflows caught.
        public static long Checked(int a, long b)
        {
            long sum = 0;
            checked
            {
                sum += a * 3;
                sum += b + 5;
                sum += (uint)a * 2u;
                try { int big = int.MaxValue; sum += big + a; } catch (OverflowException) { sum += 100; }
                try { long m = b * b * b * b; sum += m; } catch (OverflowException) { sum += 1000; }
                try { sum += (byte)(a + 200); } catch (OverflowException) { sum += 10000; }
                try { ulong u = (ulong)(b - 200000); sum += (long)u; } catch (OverflowException) { sum += 100000; }
                sum += (short)a;
                sum += (long)(ulong)b;
                sum += (int)(b - 100000 - 1);
            }
            return sum;
        }

        // Arrays of 8-byte elements.
        public static long LongArrays(int n)
        {
            long[] a = new long[n];
            double[] d = new double[n];
            for (int i = 0; i < n; i++) { a[i] = (long)i << 33; d[i] = i * 0.5; }
            long s = 0;
            for (int i = 0; i < n; i++) s += a[i] >> 33;
            double t = 0;
            foreach (double x in d) t += x;
            return s + (long)t;
        }

        // A generic class's statics: from its own shared code (hidden argument), from another generic class's
        // instance method (this object's type) and from exact code (the dynamic class ID).
        public static long SharedStatics(string a, string b)
        {
            Cache<string>.Remember(a);
            Cache<string>.Remember(b);
            int t = new Box<string>().Tag();
            return Cache<string>.Count * 100 + Cache<string>.Last.Length * 10 + t + Cache<object>.Count;
        }

        // newobj of a string: the constructor makes the object (no JIT_New).
        public static int SmallStructs(int n)
        {
            char c = (char)('a' + n);
            short s = (short)n;
            bool? maybe = n > 3;
            object boxedChar = c;
            IComparable cmp = s;
            int r = (char)boxedChar + (maybe.HasValue && maybe.Value ? 100 : 0) + cmp.CompareTo((short)2) * 10;
            Shorts t;
            t.A = (short)n; t.B = (short)(n * 2); t.C = (short)(n * 3);
            ShortsHolder holder = new ShortsHolder();
            holder.Value = t;
            Shorts[] array = new Shorts[2];
            array[1] = holder.Value;
            Shorts back = array[1];
            r += (back.A + back.B + back.C) * 1000;
            return holder.Guard == 0x11223344 ? r : -1;
        }

        // A generic struct's getter returning a struct through a buffer, from code shared over the key type: the
        // instantiation argument comes after the buffer and this.
        public static long KvpBig(string s)
        {
            Dictionary<string, Vec> d = new Dictionary<string, Vec>();
            d[s] = new Vec(1.5, 2.5);
            double t = 0;
            foreach (KeyValuePair<string, Vec> kv in d)
                t += kv.Value.X + kv.Value.Y + kv.Key.Length;
            return (long)t;
        }

        // An interface call on a generic interface from shared code (T = string): the resolver needs the exact
        // interface and method, looked up through the instantiation, not the canonical ones.
        public interface IBox<T> { T Get(); }
        public class StrBox : IBox<string> { public string Get() { return "box"; } }
        public class ObjBox : IBox<object> { public object Get() { return "boxed"; } }
        public static class Caller<T> { public static int Call(IBox<T> b) { return b.Get().ToString().Length; } }

        // ldftn from shared generic code: a delegate over a generic method's instantiation over T, whose entry point
        // comes from the generic context's dictionary at run time.
        public delegate string ShowDelegate<T>(T value);
        private static string Show<T>(T value) { return "<" + value + ">"; }
        public static class Shower<T> { public static ShowDelegate<T> Make() { return new ShowDelegate<T>(Show<T>); } }
        private static ShowDelegate<T> MakeShower<T>() { return Show<T>; }
        public static long GenericLdftn()
        {
            return Shower<string>.Make()("ab").Length + MakeShower<object>()("cde").Length * 10 + Shower<int>.Make()(7).Length * 100;
        }

        // A generic virtual method called from exact and from shared code (Iterator<T>.Select<TResult> in LINQ):
        // the implementation for the object and the exact method instantiation come from the runtime's helper.
        public abstract class Shape<T>
        {
            public T Value;
            public virtual string Describe<U>(U extra) { return "shape " + Value + " " + extra; }
        }
        public class Square<T> : Shape<T>
        {
            public override string Describe<U>(U extra) { return "square " + Value + " " + extra; }
        }
        public static class ShapeUser<T>
        {
            public static int Use(Shape<T> s) { return s.Describe("x").Length + s.Describe(12).Length * 100; }
        }
        public static long GenericVirtual()
        {
            Shape<string> a = new Square<string> { Value = "ab" };
            Shape<int> b = new Square<int> { Value = 7 };
            return a.Describe(3.5).Length + ShapeUser<string>.Use(a) * 10 + ShapeUser<int>.Use(b) * 10000;
        }

        // Floating-point arguments whose slots are on the stack (the ninth and tenth), caller and callee compiled.
        public static double Sum10D(int a, int b, int c, int d, int e, int f, int g, int h, double i, double j)
        {
            return a + b + c + d + e + f + g + h + i * 100 + j * 1000;
        }
        public static long FloatStackArgs()
        {
            return (long)(Sum10D(1, 2, 3, 4, 5, 6, 7, 8, 1.5, 2.25) + Sum10D(0, 0, 0, 0, 0, 0, 0, 0, 0.5, 0.125));
        }

        // A 136-byte value type (System.Windows's Matrix3D's size): as a local, an argument, a result and a field.
        public struct Big16
        {
            public double A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P;
            public int Tag, Tag2;
            public static Big16 Make(double v, int tag) { Big16 b = new Big16(); b.A = v; b.P = v * 2; b.Tag = tag; return b; }
            public double Sum() { return A + B + C + D + E + F + G + H + I + J + K + L + M + N + O + P + Tag + Tag2; }
        }
        public class BigHolder { public Big16 Value; }
        private static double BigThrough(Big16 b, int k) { b.Tag2 = k; return b.Sum(); }
        public static long BigStruct()
        {
            Big16 b = Big16.Make(1.5, 10);
            var h = new BigHolder { Value = b };
            h.Value.B = 4;
            return (long)(b.Sum() + BigThrough(h.Value, 100) * 100 + Big16.Make(2, 1).Sum() * 10000);
        }

        // A value type over 136 bytes (Microsoft.Xbox's marshaling stubs copy 0x130-byte MediaState): 328 bytes with a
        // reference first, as a local, a by-value argument (on the stack past r10), a result through a buffer, a field
        // and an array element, with the GC running while only the copies hold the string.
        public struct Huge
        {
            public string Name;
            public double D0, D1, D2, D3, D4, D5, D6, D7, D8, D9, D10, D11, D12, D13, D14, D15, D16, D17, D18, D19;
            public double D20, D21, D22, D23, D24, D25, D26, D27, D28, D29, D30, D31, D32, D33, D34, D35, D36, D37, D38, D39;
            public static Huge Make(int n)
            {
                Huge h = new Huge();
                h.Name = new string('h', n);
                h.D0 = n;
                h.D39 = n * 2;
                return h;
            }
            public double Sum() { return Name.Length + D0 + D1 + D20 + D39; }
        }
        public class HugeHolder { public Huge Value; }
        private static double HugeThrough(Huge h, int k)
        {
            h.D1 = k;
            GC.Collect();
            return h.Sum();
        }
        public static long HugeStruct()
        {
            Huge h = Huge.Make(3);
            var holder = new HugeHolder { Value = Huge.Make(5) };
            holder.Value.D20 = 7;
            Huge[] array = new Huge[2];
            array[1] = h;
            Huge copy = holder.Value;
            GC.Collect();
            return (long)(h.Sum() + HugeThrough(copy, 100) * 100 + array[1].Sum() * 10000 + holder.Value.Name.Length * 1000000);
        }

        // TypedReference: __makeref, __reftype (refanytype) and the runtime's TypedReference.GetHashCode (an ldobj of
        // the TypedReference, then refanytype).
        public static long TypedRef()
        {
            int value = 5;
            TypedReference tr = __makeref(value);
            Type t = __reftype(tr);
            return (t == typeof(int) ? 1 : 0) + (tr.GetHashCode() == typeof(int).GetHashCode() ? 10 : 0);
        }

        // A return from a try/finally inside a catch, inside an outer try/finally: the inner finally runs in the catch
        // funclet, then the catch returns to the outer finally's thunk (HLSPlayback.DataStreamResponseReceived).
        private static int leaveTrace;
        private static int LeaveThroughCatch(int x)
        {
            try
            {
                try
                {
                    if (x > 0) throw new InvalidOperationException("x");
                    leaveTrace += 1;
                }
                catch (InvalidOperationException)
                {
                    try
                    {
                        leaveTrace += 10;
                        if (x > 1) return leaveTrace;
                    }
                    finally
                    {
                        leaveTrace += 100;
                    }
                    leaveTrace += 1000;
                }
            }
            finally
            {
                leaveTrace += 10000;
            }
            return -1;
        }
        public static long LeaveCatchFinally()
        {
            leaveTrace = 0;
            long early = LeaveThroughCatch(2) * 100000L + leaveTrace;
            leaveTrace = 0;
            return early * 100 + LeaveThroughCatch(1) + leaveTrace;
        }

        // `ref a[i, j]` of multi-dimensional arrays: the array's Address accessor, which takes the array's exact type as
        // a hidden argument after this (RijndaelEngine.GenerateWorkingKey's long[,]; the string[,] one checks it).
        private static void Bump(ref long x, long by) { x += by; }
        private static void SetText(ref string x, string text) { x = text; }
        public static long MdArrayAddress()
        {
            var g = new long[3, 4];
            Bump(ref g[1, 2], 5);
            Bump(ref g[2, 3], 7);
            g[1, 2] *= 10;
            var s = new string[2, 2];
            SetText(ref s[1, 0], "abc");
            return g[1, 2] * 1000 + g[2, 3] + s[1, 0].Length * 100000;
        }

        // Frameless leaf methods: arguments and locals in registers. Small integer arguments and locals keep their
        // types' ranges (the prolog narrows what arrives, a store narrows what goes in); the third argument and on
        // move out of the evaluation stack's registers.
        private static int LeafNarrow(sbyte a, byte b, short c, char d)
        {
            return (sbyte)(a + 100) + (byte)(b + 200) + (short)(c * 300) + (char)(d + 1);
        }
        private static long LeafMix(int a, int b, int c, int d, long e)
        {
            int t = a - b * 2;
            long u = t + c * 3 - d * 4;
            return u + e;
        }
        public static long LeafRegisters()
        {
            return LeafNarrow(100, 100, 200, 'A') * 1000L + LeafMix(1, 2, 3, 4, 5000000000L);
        }

        // Variables in callee-saved registers across calls: references and a byref among them reported as register
        // slots, here across collections that may move what they point to.
        private static int[] SavedGarbage(int n)
        {
            int[] a = null;
            for (int i = 0; i < n; i++)
                a = new int[16];
            return a;
        }
        private static int SavedByref(ref int cell, int rounds)
        {
            int sum = 0;
            for (int i = 0; i < rounds; i++)
            {
                cell += i;
                SavedGarbage(200);
                if ((i & 3) == 0)
                    GC.Collect();
                sum += cell;
            }
            return sum;
        }
        public static long SavedRegistersGC()
        {
            string text = "saved" + 42;
            int[] cells = new int[8];
            object[] keep = new object[4];
            long total = 0;
            for (int i = 0; i < 12; i++)
            {
                keep[i & 3] = SavedGarbage(50);
                cells[i & 7] += SavedByref(ref cells[(i + 1) & 7], 6);
                if ((i & 3) == 0)
                    GC.Collect();
                total += text.Length + cells[i & 7] + ((int[])keep[i & 3]).Length;
            }
            return total;
        }

        // Reference stores through the per-register write barriers (the value in a cache or variable register, the
        // entries under it left in theirs): young objects reachable only from fields of an object collected into an
        // older generation survive the collections the allocations then cause only if the barrier marked its card.
        public class BarrierHolder
        {
            public object A, B, C, D;
            public string S;
        }
        public static int BarrierRegisters(int n)
        {
            BarrierHolder old = new BarrierHolder();
            GC.Collect();
            GC.Collect();
            int sum = 0;
            for (int i = 0; i < n; i++)
            {
                string s = "s" + i;
                object a = new int[] { i }, b = new int[] { i + 1 };
                old.A = a;
                old.B = b;
                old.S = s;
                old.C = new int[] { i + 2 };
                old.D = old.C;
                a = b = null;
                s = null;
                SavedGarbage(3000);
                sum += ((int[])old.A)[0] + ((int[])old.B)[0] + ((int[])old.C)[0] + ((int[])old.D)[0] + old.S.Length;
            }
            return sum;
        }

        // P/Invoke calls while another thread's allocations make the GC run: the caller's references live in callee-
        // saved registers across the native call, where the GC finds them through the P/Invoke stub's saves or the
        // runtime's transition frame (it passes without the former too: docs/RESEARCH.md). Each call sleeps a
        // millisecond (KeDelayExecutionThread, xboxkrnl ordinal 90, a relative interval in 100 ns units), so
        // collections can land inside it.
        [System.Runtime.InteropServices.DllImport("xboxkrnl.exe", EntryPoint = "#90")]
        private static extern int KeDelayExecutionThread(int mode, int alertable, ref long interval);
        private static volatile bool churning;
        public static int PInvokeUnderGC(int rounds)
        {
            churning = true;
            var churn = new System.Threading.Thread(() =>
            {
                while (churning)
                    SavedGarbage(1000);
            });
            churn.Start();
            string s = "pinv" + rounds;
            int[] cell = new int[] { 7 };
            object boxes = new object[] { "x" };
            int sum = 0;
            for (int i = 0; i < rounds; i++)
            {
                long interval = -10000;
                int status = KeDelayExecutionThread(1, 0, ref interval);
                sum += (status == 0 ? 1 : 0) + s.Length + cell[0] + ((object[])boxes).Length;
                if ((i & 15) == 0)
                    cell = new int[] { cell[0] };
            }
            churning = false;
            churn.Join();
            return sum;
        }

        // Results computed straight into the callee-saved register of the variable the next instruction stores them
        // to: narrowing stores, the target among the operands, a dup into two variables, a call's result.
        public static long FusedStores(int n)
        {
            byte b = 0;
            short sh = 0;
            int x = 1, y = 2;
            long w = 0;
            string s = null;
            for (int i = 0; i < n; i++)
            {
                b = (byte)(b + 200);
                sh = (short)(sh * 3 + 7);
                x = y - x;
                y = x = x + y + i;
                w = w * 31 + SavedId(x);
                s = i.ToString();
            }
            return b + sh * 1000L + x * 10L + y + w + s.Length;
        }

        // A method with handlers keeps the variables its handlers don't use in callee-saved registers (caught and
        // total, used in the catch and the finally, stay in the frame): the callee clobbers those registers and
        // throws, the GC runs in the catch, and the registers come back as they were when the method resumes.
        public static long SavedAcrossHandlers(int n)
        {
            int a = n + 1, b = n * 3;
            long big = 1L << 33;
            string s = "abc" + n;
            object[] keep = new object[] { "k" };
            long total = 0;
            int caught = 0;
            for (int i = 0; i < 6; i++)
            {
                try
                {
                    total += ClobberAndThrow(i - 3);
                    total += a + b;
                }
                catch (InvalidOperationException)
                {
                    caught++;
                    GC.Collect();
                }
                finally
                {
                    total += 1000;
                }
                total += a * 10 + b + big + s.Length + ((string)keep[0]).Length;
            }
            return total + caught * 100000;
        }

        // Floating-point variables in f31 down to f14: a callee with its own there (FpClobber) throws into a method
        // with handlers whose p, q, f and sum live there too, restored by the unwinder; caught stays in the frame.
        private static double SavedIdD(double v) { return v; }
        private static double FpClobber(double x)
        {
            double a = x * 2, b = x * 3;
            for (int i = 0; i < 3; i++)
            {
                a = a * 1.5 + SavedIdD(b);
                b += a;
            }
            if (x >= 0)
                throw new InvalidOperationException("clobbered " + (a + b));
            return a + b;
        }
        public static long SavedFloats(int n)
        {
            double p = n * 0.5, q = n + 0.25;
            float f = n * 1.5f;
            double sum = 0;
            int caught = 0;
            for (int i = 0; i < 4; i++)
            {
                try
                {
                    sum += FpClobber(i - 2);
                }
                catch (InvalidOperationException)
                {
                    caught++;
                }
                sum += p * 4 + q * 8 + f * 2;
            }
            return (long)(sum * 1000) + caught;
        }

        static double Twice(double x)
        {
            return x + x;
        }

        // Floating-point variables in callee-saved registers, read where they live: an entry naming a variable's
        // register while the variable is stored to (b + (b = b * 2)), a result computed straight into a variable's
        // register, a dup of an entry naming one (p = q = ...), such an entry across a call that stores the variable
        // in its argument (a + Twice(a = a + 1)), and a float variable rounded to single precision when stored.
        public static long AliasedFloats(int n)
        {
            double a = 1, b = 2, p = 0, q = 0, d = 0;
            float f = 1;
            for (int i = 0; i < n; i++)
            {
                double t = b + (b = b * 2);
                a = a * 2 + t;
                p = q = a - b;
                d = a + Twice(a = a + 1);
                f = f / 3;
            }
            return (long)(a + b * 3 + p * 5 + q * 7 + d * 11) * 100000000000000L + (long)(f * 1e12);
        }

        class StringBox
        {
            public string S;
        }

        // A reference stored to a field while a computed value is still in r3 below it: the write barrier takes the
        // destination in r3 (and the value in r4), so the entries there are written back first.
        public static long BarrierBelow(int n)
        {
            StringBox h = new StringBox();
            long r = 0;
            for (int i = 0; i < n; i++)
            {
                string s = i.ToString();
                int t = (i * 3) + ((h.S = s) == null ? 100 : s.Length);
                r = r * 7 + t;
            }
            return r * 10 + (h.S == null ? 0 : h.S.Length);
        }

        // Leaf methods with floating point: arguments in f1 and f13 and above the stack's registers, integer
        // arguments among them moved all at once, constants from the read-only data, a float result rounded.
        static double LeafFp(double a, double b, int n, double c)
        {
            return (n > 0 ? a : b) * c + (a - b);
        }

        static int LeafCompare(double a, int x, double b, int y)
        {
            return a < b ? x - y : y - x;
        }

        static float LeafFloat(float f, double d)
        {
            return f * 2 + (float)d;
        }

        static double LeafConst(double x)
        {
            return x * 0.5 + 1.25;
        }

        public static long LeafFloats()
        {
            double sum = LeafFp(1.5, 2.25, 1, 4) + LeafFp(1.5, 2.25, -1, 4) + LeafFloat(1.5f, 0.25) + LeafConst(3.0);
            return (long)(sum * 100) + LeafCompare(1.0, 10, 2.0, 3) * 1000 + LeafCompare(3.0, 10, 2.0, 3) * 10;
        }

        struct Span8
        {
            public long Ticks;
        }

        struct Mixed8
        {
            public int X;
            public byte Y;
            public short Z;
        }

        struct Pair4
        {
            public short A;
            public short B;
        }

        class SpanBox
        {
            public Span8 S;
        }

        // Leaf methods with 4- and 8-byte value types in registers: arguments read through ldarga + ldfld (fields
        // rotated out of the register, sign-extended), a value type returned in r3, one loaded from an object's field.
        static bool LeafEq(Span8 a, Span8 b)
        {
            return a.Ticks == b.Ticks;
        }

        static int LeafMixed(Mixed8 m)
        {
            return m.X + m.Y * 1000 + m.Z * 100000;
        }

        static int LeafPair(Pair4 p)
        {
            return p.A * 1000 + p.B;
        }

        static Span8 LeafGet(SpanBox b)
        {
            return b.S;
        }

        static Mixed8 LeafSame(Mixed8 m)
        {
            return m;
        }

        public static long LeafStructs()
        {
            Span8 a = new Span8 { Ticks = 0x123456789AL }, b = new Span8 { Ticks = 0x123456789AL }, c = new Span8 { Ticks = 5 };
            Mixed8 m = new Mixed8 { X = -7, Y = 200, Z = -3 };
            Pair4 p = new Pair4 { A = -12, B = -34 };
            SpanBox box = new SpanBox { S = c };
            long r = (LeafEq(a, b) ? 1 : 0) + (LeafEq(a, c) ? 10 : 0);
            r += (long)LeafMixed(LeafSame(m)) * 100;
            r += (long)LeafPair(p) * 10000000000L;
            r += LeafGet(box).Ticks * 1000000000000000L;
            return r;
        }

        struct Int4
        {
            public int V;
        }

        struct Dbl8
        {
            public double D;
        }

        static Int4 MakeInt4(int v)
        {
            Int4 x;
            x.V = v;
            return x;
        }

        static int ReadInt4(Int4 x)
        {
            return x.V * 3;
        }

        static double ReadDbl8(Dbl8 d)
        {
            return d.D * 2;
        }

        // Value types of one primitive field, read from the value (ldarg + ldfld, a call's result + ldfld): one of a
        // long the runtime reports as a CORINFO_TYPE_LONG (LeafEq above), one of an int or a double as VALUECLASS.
        public static long OneFieldStructs()
        {
            Dbl8 d = new Dbl8 { D = 1.25 };
            long r = ReadInt4(MakeInt4(7)) + MakeInt4(40).V * 100;
            r += (long)(ReadDbl8(d) * 10000);
            return r;
        }

        // stelem.ref's paths: an object of the element class exactly (inline, the barrier for r5), null (no barrier),
        // a subtype or anything into object[] (the helper), a mismatch through a covariant array (the helper throws),
        // an index out of range; and young strings stored the inline way into an old array, kept alive by its cards.
        public static int StelemRefPaths()
        {
            string[] strings = new string[3];
            object[] objects = new object[3];
            object[] covariant = strings;
            strings[0] = "a";
            strings[1] = null;
            objects[0] = "b";
            objects[1] = objects;
            covariant[2] = "c";
            int r = 0;
            try
            {
                covariant[0] = new object();
            }
            catch (ArrayTypeMismatchException)
            {
                r += 100;
            }
            try
            {
                strings[3] = "x";
            }
            catch (IndexOutOfRangeException)
            {
                r += 1000;
            }
            GC.Collect();
            return r + strings[0].Length + (strings[1] == null ? 10 : 0) + ((string)objects[0]).Length +
                strings[2].Length * 20 + (objects[1] == objects ? 5 : 0) + OldArrayCards();
        }
        private static int OldArrayCards()
        {
            string[] old = new string[8];
            GC.Collect();
            GC.Collect();
            for (int i = 0; i < old.Length; i++)
            {
                old[i] = "s" + (i * 11);
                SavedGarbage(2000);
            }
            int sum = 0;
            for (int i = 0; i < old.Length; i++)
                sum += old[i].Length;
            return sum * 10000;
        }

        // An exception thrown from a method that saved its caller's callee-saved registers and used them, caught by
        // a method between: the unwinder restores them from the thrower's prolog saves, so the outer loop's
        // variables (in the same registers) come back intact.
        private static int SavedId(int x) { return x; }
        private static int ClobberAndThrow(int n)
        {
            int x = n * 7, y = n * 11, z = n * 13;
            for (int i = 0; i < 4; i++)
            {
                x += SavedId(y);
                y += SavedId(z);
                z += SavedId(x);
            }
            if (n >= 0)
                throw new InvalidOperationException("clobbered " + (x + y + z));
            return x + y + z;
        }
        private static int CatchClobber(int n)
        {
            try
            {
                return ClobberAndThrow(n);
            }
            catch (InvalidOperationException)
            {
                return 1;
            }
        }
        public static long UnwindSaved(int n)
        {
            int a = n + 1, b = n + 2, c = n + 3;
            long big = 3000000000L + n;
            string s = "abcd";
            long total = 0;
            for (int i = 0; i < 4; i++)
            {
                total += CatchClobber(i);
                total += a * 1000 + b * 100 + c * 10 + s.Length + big;
            }
            return total;
        }

        // An exception type of this assembly whose inherited Message (an import from mscorlib, bound on first use) is
        // first called inside a catch handler, the exception thrown by a callee (Silverdash360's DebugServer.Handle
        // died this way on our image). Run first, before anything else reads a Message.
        public class BenchException : Exception
        {
            public BenchException(string message) : base(message) { }
        }
        private static void ThrowBench(string text) { throw new BenchException(text + "!"); }
        public static string CatchFirstMessage(string text)
        {
            string reply = "none";
            try
            {
                ThrowBench(text);
            }
            catch (Exception e)
            {
                reply = e is BenchException ? e.Message : e.GetType().Name + ": " + e.Message;
            }
            return reply;
        }

        // [ThreadStatic] fields of a class with a class constructor (helpers 373/374; Silverdash360's Log): each thread
        // its own, starting at the default, the class constructor run first.
        public static class ThreadStaticWithCctor
        {
            public static readonly List<int> Seen = new List<int> { 7 };
            [ThreadStatic] public static bool Flag;
            [ThreadStatic] public static int Count;
            [ThreadStatic] public static string Name;
        }
        public static long ThreadStaticCctor()
        {
            ThreadStaticWithCctor.Flag = true;
            ThreadStaticWithCctor.Count = 5;
            ThreadStaticWithCctor.Name = "main";
            long other = -1;
            var t = new System.Threading.Thread(() =>
            {
                other = (ThreadStaticWithCctor.Flag ? 1 : 0) + ThreadStaticWithCctor.Count * 10 +
                    (ThreadStaticWithCctor.Name == null ? 100 : 0);
                ThreadStaticWithCctor.Count = 9;
            });
            t.Start();
            t.Join();
            return other * 10000 + (ThreadStaticWithCctor.Flag ? 1 : 0) + ThreadStaticWithCctor.Count * 10 +
                ThreadStaticWithCctor.Name.Length * 100 + ThreadStaticWithCctor.Seen[0] * 1000;
        }

        // A generic method returning a value type through a buffer, called from shared generic code.
        private static Vec MakeVec<T>(T item) { return new Vec(item.ToString().Length, 2); }
        public static class VecCaller<T> { public static double Call(T item) { return MakeVec(item).Length2; } }
        public static long GenericRetBuf()
        {
            return (long)(VecCaller<string>.Call("abc") + VecCaller<object>.Call(12345) * 100);
        }

        // An exception's message: looked up in mscorlib's resources, through the satellite assembly search for the
        // UI culture (HelloXbox died in it on our images, formatting a WebException's message).
        // Where mscorlib's message lookup goes: the UI culture, the manifest resource, a ResourceManager's answer.
        public static string ResourceProbe()
        {
            var r = new System.Text.StringBuilder();
            try { r.Append("ui=" + System.Globalization.CultureInfo.CurrentUICulture.Name + " cur=" + System.Globalization.CultureInfo.CurrentCulture.Name); } catch (Exception e) { r.Append("culture:" + e.GetType().Name); }
            try {
                var asm = typeof(object).Assembly;
                var names = asm.GetManifestResourceNames();
                r.Append(" names=" + names.Length + (names.Length > 0 ? "(" + names[0] + ")" : ""));
                using (var st = asm.GetManifestResourceStream("mscorlib.resources")) r.Append(" stream=" + (st == null ? "null" : st.Length.ToString()));
            } catch (Exception e) { r.Append(" manifest:" + e.GetType().Name); }
            try {
                var rm = new System.Resources.ResourceManager("mscorlib", typeof(object).Assembly);
                string v = rm.GetString("Arg_InvalidOperationException", System.Globalization.CultureInfo.InvariantCulture);
                r.Append(" inv='" + v + "'");
                v = rm.GetString("Arg_InvalidOperationException");
                r.Append(" cur='" + v + "'");
            } catch (Exception e) { r.Append(" rm:" + e.GetType().Name + ":" + e.Message); }
            return r.ToString();
        }

        public static long ExceptionMessage()
        {
            string a = "", b = "", c = "";
            try { throw new InvalidOperationException(); } catch (InvalidOperationException e) { a = e.Message; }
            try { int[] x = new int[1]; x[2] = 1; } catch (IndexOutOfRangeException e) { b = e.Message; }
            try { object o = "s"; int n = (int)o; } catch (InvalidCastException e) { c = e.Message; }
            return (a.Length > 0 ? 1 : 0) + (b.Length > 0 ? 10 : 0) + (c.Length > 0 ? 100 : 0);
        }

        public static long GenericIface()
        {
            return Caller<string>.Call(new StrBox()) + Caller<object>.Call(new ObjBox()) * 10;
        }

        // A dictionary over a string key and a struct value holding a reference (mscorlib's ResourceLocator in
        // RuntimeResourceSet), filled, then a GC, then looked up: shared code over the key, a struct with a reference
        // in each entry.
        public struct Loc { public object Value; public int Pos; }

        public static long DictStructRef(int n)
        {
            Dictionary<string, Loc> d = new Dictionary<string, Loc>();
            for (int i = 0; i < n; i++)
            {
                Loc l = new Loc();
                l.Value = "v" + i;
                l.Pos = i;
                d["k" + i] = l;
            }
            Garbage(3000);
            long t = 0;
            for (int i = 0; i < n; i++)
            {
                Loc l;
                if (!d.TryGetValue("k" + i, out l))
                    t -= 1000 * (i + 1);
                else if (l.Pos != i)
                    t -= 100000 * (i + 1);
                else if (!(l.Value is string) || (string)l.Value != "v" + i)
                    t -= 10000000 * (i + 1);
                else
                    t += l.Pos + ((string)l.Value).Length;
            }
            return t;
        }

        // Arrays of structs holding a struct with a reference, as Dictionary's Entry<TKey, TValue> (a generic struct:
        // auto layout, references first) holds its value: stores through ldelema must not touch the neighbours.
        public struct Ent { public int hash; public int next; public string key; public Loc value; }
        public struct GEnt<K, V> { public int hash; public int next; public K key; public V value; }

        public static long EntryArray()
        {
            Ent[] a = new Ent[4];
            for (int i = 0; i < 4; i++)
            {
                a[i].hash = 100 + i; a[i].next = i - 1; a[i].key = "k" + i;
                Loc l; l.Value = "v" + i; l.Pos = i * 7; a[i].value = l;
            }
            long t = 0;
            for (int i = 0; i < 4; i++)
                t += a[i].hash * 1000 + a[i].next * 100 + a[i].key.Length + a[i].value.Pos + ((string)a[i].value.Value).Length;
            return t;
        }

        public static long GenericEntryArray()
        {
            GEnt<string, Loc>[] a = new GEnt<string, Loc>[4];
            for (int i = 0; i < 4; i++)
            {
                a[i].hash = 100 + i; a[i].next = i - 1; a[i].key = "k" + i;
                Loc l; l.Value = "v" + i; l.Pos = i * 7; a[i].value = l;
            }
            long t = 0;
            for (int i = 0; i < 4; i++)
                t += a[i].hash * 1000 + a[i].next * 100 + a[i].key.Length + a[i].value.Pos + ((string)a[i].value.Value).Length;
            return t;
        }

        static int DictProbe(int n, bool gc)
        {
            Dictionary<string, Loc> d = new Dictionary<string, Loc>();
            for (int i = 0; i < n; i++) { Loc l = new Loc(); l.Value = "v" + i; l.Pos = i; d["k" + i] = l; }
            if (gc) Garbage(3000);
            int found = 0;
            for (int i = 0; i < n; i++) { Loc l; if (d.TryGetValue("k" + i, out l) && l.Pos == i) found++; }
            return found == n ? 1 : 0;
        }

        // Bit i set when variant i keeps every key: n = 1, 3 (no resize), 4 (one resize), 10, and 10 with a GC.
        public static long DictVariants()
        {
            return DictProbe(1, false) | DictProbe(3, false) << 1 | DictProbe(4, false) << 2 | DictProbe(10, false) << 3 |
                DictProbe(10, true) << 4 | (DictProbe(1, true) << 5);
        }

        // Dictionary.Resize's copy and rehash over GEnt<string, Loc>[]: bit i set when entry i is reachable after.
        public static long ResizeProbe()
        {
            GEnt<string, Loc>[] e = new GEnt<string, Loc>[3];
            for (int i = 0; i < 3; i++) { e[i].hash = 100 + i; e[i].next = -1; e[i].key = "k" + i; Loc l; l.Value = "v" + i; l.Pos = i; e[i].value = l; }
            int[] nb = new int[7];
            for (int i = 0; i < 7; i++) nb[i] = -1;
            GEnt<string, Loc>[] ne = new GEnt<string, Loc>[7];
            Array.Copy(e, 0, ne, 0, 3);
            for (int i = 0; i < 3; i++)
            {
                if (ne[i].hash >= 0) { int b = ne[i].hash % 7; ne[i].next = nb[b]; nb[b] = i; }
            }
            int ok = 0;
            for (int i = 0; i < 3; i++)
            {
                int b = (100 + i) % 7;
                for (int j = nb[b]; j >= 0; j = ne[j].next)
                    if (ne[j].hash == 100 + i && ne[j].key == "k" + i && ne[j].value.Pos == i) ok |= 1 << i;
            }
            return ok;
        }

        // After one resize: Count * 100 + keys seen by enumeration (which walks entries, not buckets) + 10 if k0 is
        // found through the buckets.
        public static long DictAfterResize()
        {
            Dictionary<string, Loc> d = new Dictionary<string, Loc>();
            for (int i = 0; i < 4; i++) { Loc l = new Loc(); l.Value = "v" + i; l.Pos = i; d["k" + i] = l; }
            int seen = 0;
            foreach (KeyValuePair<string, Loc> kv in d)
                if (kv.Key == "k" + kv.Value.Pos) seen++;
            Loc o;
            return d.Count * 100 + seen + (d.TryGetValue("k0", out o) ? 10 : 0);
        }

        // Array.Copy by itself: an int array copied whole (digits of the copy), then GEnt<string, Loc> entries
        // (bit i set when entry i arrived intact), then the same from index 1 (bit 4 set when it arrived).
        public static long ArrayCopyProbe()
        {
            int[] a = { 1, 2, 3, 4 };
            int[] b = new int[4];
            Array.Copy(a, 0, b, 0, 4);
            long digits = b[0] * 1000 + b[1] * 100 + b[2] * 10 + b[3];
            GEnt<string, Loc>[] e = new GEnt<string, Loc>[3];
            for (int i = 0; i < 3; i++) { e[i].hash = 100 + i; e[i].next = -1; e[i].key = "k" + i; Loc l; l.Value = "v" + i; l.Pos = i; e[i].value = l; }
            GEnt<string, Loc>[] ne = new GEnt<string, Loc>[7];
            Array.Copy(e, 0, ne, 0, 3);
            int bits = 0;
            for (int i = 0; i < 3; i++)
                if (ne[i].hash == 100 + i && ne[i].next == -1 && ne[i].key == "k" + i && ne[i].value.Pos == i && (string)ne[i].value.Value == "v" + i)
                    bits |= 1 << i;
            // Which fields of the first copied entry arrived: hash 8, next 16, key 32, value.Pos 64, value.Value 128.
            if (ne[0].hash == 100) bits |= 8;
            if (ne[0].next == -1) bits |= 16;
            if (ne[0].key == "k0") bits |= 32;
            if (ne[0].value.Pos == 0) bits |= 64;
            if ((string)ne[0].value.Value == "v0") bits |= 128;
            // A struct array without references, and a non-generic one with them: bit 8/9 set when entry 0 arrives.
            GEnt<int, int>[] p = new GEnt<int, int>[2]; p[0].hash = 7; p[0].key = 8; p[0].value = 9;
            GEnt<int, int>[] q = new GEnt<int, int>[2];
            Array.Copy(p, 0, q, 0, 2);
            if (q[0].hash == 7 && q[0].key == 8 && q[0].value == 9) bits |= 256;
            Ent[] r = new Ent[2]; r[0].hash = 5; r[0].key = "a"; Loc m; m.Value = "b"; m.Pos = 3; r[0].value = m;
            Ent[] t = new Ent[2];
            Array.Copy(r, 0, t, 0, 2);
            if (t[0].hash == 5 && t[0].key == "a" && t[0].value.Pos == 3 && (string)t[0].value.Value == "b") bits |= 512;
            // The arrays' own types, as the runtime sees them (their MethodTable): bits 10-12.
            if (((object)new Ent[2]).GetType() == typeof(Ent[])) bits |= 1024;
            if (((object)new GEnt<int, int>[2]).GetType() == typeof(GEnt<int, int>[])) bits |= 2048;
            if (((object)new int[2]).GetType() == typeof(int[])) bits |= 4096;
            return digits * 10000 + bits;
        }

        // List<T>'s growth as shared code over a reference type (mscorlib's List<RuntimeEventInfo>.Add threw
        // IndexOutOfRange from the store after growing): new T[] through a runtime lookup, Array.Copy, stelem.ref.
        public class MyList<T>
        {
            T[] _items = new T[0];
            int _size;
            public void Add(T item) { if (_size == _items.Length) EnsureCapacity(_size + 1); _items[_size++] = item; }
            void EnsureCapacity(int min)
            {
                if (_items.Length < min)
                {
                    int nc = _items.Length == 0 ? 4 : _items.Length * 2;
                    if (nc < min) nc = min;
                    T[] n = new T[nc];
                    if (_size > 0) Array.Copy(_items, 0, n, 0, _size);
                    _items = n;
                }
            }
            public int Count { get { return _size; } }
            public int Capacity { get { return _items.Length; } }
            public T this[int i] { get { return _items[i]; } }
        }

        public static long MyListTest(int n)
        {
            MyList<string> l = new MyList<string>();
            for (int i = 0; i < n; i++) l.Add("s" + i);
            long t = 0;
            for (int i = 0; i < l.Count; i++) t += l[i].Length;
            return t * 100 + l.Capacity;
        }

        // unbox.any of a Nullable<T> from a boxed T, from null, and in a generic method.
        static int UnboxNullable<T>(object o) where T : struct { T? v = (T?)o; return v.HasValue ? 1 : 0; }

        public static long NullableUnbox(int n)
        {
            object boxed = n;
            object none = null;
            int? a = (int?)boxed;
            int? b = (int?)none;
            long? c = (long?)(object)(long)n;
            return (a.HasValue ? a.Value : 0) + (b.HasValue ? 1000 : 0) + (c.HasValue ? (long)c.Value * 10 : 0) +
                UnboxNullable<int>(boxed) * 100 + UnboxNullable<double>(none) * 100000;
        }

        // Reflection over a type's events (what the XAML parser does to attach handlers), with a GC between
        // lookups: mscorlib's MemberInfoCache grows List<RuntimeEventInfo> while populating.
        public delegate void Handler(int x);
        public class Eventful
        {
            public event Handler E0, E1, E2, E3, E4, E5, E6, E7, E8, E9, E10, E11, E12, E13, E14, E15, E16, E17, E18, E19;
            public void Fire() { if (E0 != null) E0(0); if (E19 != null) E19(19); }
        }

        public class Eventful<T>
        {
            public event Handler E0, E1, E2, E3, E4, E5, E6, E7, E8, E9, E10, E11, E12, E13, E14, E15, E16, E17, E18, E19;
            public event Handler F0, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15, F16, F17, F18, F19;
            public void Fire() { if (E0 != null) E0(0); if (F19 != null) F19(19); }
        }

        static int CountEvents(Type t, int garbage)
        {
            Garbage(garbage);
            System.Reflection.EventInfo[] events = t.GetEvents();
            System.Reflection.EventInfo e = t.GetEvent("F7");
            return events.Length + (e != null && e.Name == "F7" ? 100 : 0);
        }

        // Many distinct types (instantiations), each populated once, with the heap left in a different state before
        // each: a GC during the population of some of them.
        // Floating-point remainder and checked conversions from double (the runtime's helpers), the narrowing ones
        // through the integer checks too. d = 10.5.
        public static long FloatRemConv(double d)
        {
            double r = d % 3.0;                       // 1.5
            double neg = -d % 4.0;                    // -2.5
            long big = Convert.ToInt64(d * 1e6);      // 10500000
            int small = checked((int)(d * 10));       // 105
            ulong u = checked((ulong)(d * 100));      // 1050
            byte b = checked((byte)d);                // 10
            int overflow = 0;
            try { overflow = checked((int)(d * 1e9)); } catch (OverflowException) { overflow = 1; }
            try { overflow += (int)checked((long)(d * 1e19)); } catch (OverflowException) { overflow += 2; }
            try { overflow += checked((byte)(d * 100)); } catch (OverflowException) { overflow += 4; }
            return (long)(r * 10) + (long)(neg * 10) * 100 + big + small * 1000 + (long)u * 100000 + b * 10 +
                overflow * 1000000;
        }

        // Where the runtime finds each array's data: Array.Clear(array, 0, 1) through the runtime clears the first
        // element at its offset; the element our code then sees cleared tells whether we agree ("0" = element 0).
        public static string ArrayClearProbe()
        {
            long[] l = { 1, 2, 3 };
            double[] d = { 1.5, 2.5, 3.5 };
            int[] i = { 1, 2, 3 };
            GEnt<int, int>[] s = new GEnt<int, int>[3];
            for (int k = 0; k < 3; k++) s[k].hash = k + 1;
            string[] r = { "a", "b", "c" };
            Pixel[] p4 = new Pixel[3];
            for (int k = 0; k < 3; k++) p4[k].Value = k + 1;
            Pair8[] p8 = new Pair8[3];
            for (int k = 0; k < 3; k++) p8[k].A = k + 1;
            Triple[] p12 = new Triple[3];
            for (int k = 0; k < 3; k++) p12[k].A = k + 1;
            Array.Clear(l, 0, 1); Array.Clear(d, 0, 1); Array.Clear(i, 0, 1); Array.Clear(s, 0, 1); Array.Clear(r, 0, 1);
            Array.Clear(p4, 0, 1); Array.Clear(p8, 0, 1); Array.Clear(p12, 0, 1);
            return "long " + l[0] + l[1] + l[2] + " double " + d[0] + d[1] + d[2] + " int " + i[0] + i[1] + i[2] +
                " struct " + s[0].hash + s[1].hash + s[2].hash + " string " + (r[0] ?? "-") + (r[1] ?? "-") + (r[2] ?? "-") +
                " s4 " + p4[0].Value + p4[1].Value + p4[2].Value + " s8 " + p8[0].A + p8[1].A + p8[2].A +
                " s12 " + p12[0].A + p12[1].A + p12[2].A;
        }

        // The runtime's data offset of each array type: element 0's first word is set to a marker through this
        // code's element addressing (which starts at ARRAY_DATA_OURS for these), then the words from a pinned
        // handle's address (the runtime's data start) are scanned for it: the runtime's offset is ours less the
        // marker's distance.
        public struct S1 { public byte A; }
        public struct S2 { public short A; }
        public struct S8L { public long A; }
        public struct S16 { public int A, B, C, D; }
        public struct S16D { public double A, B; }
        public struct S20 { public int A, B, C, D, E; }
        public struct S24L { public long A, B, C; }
        public struct S12G<T> { public int A; public T B; public int C; }
        // Every field of three elements set through this code's element addressing (which starts at +8 for a
        // value type without references, +16 with), the runtime's Array.Clear(array, 0, 1) zeroing one element
        // at its own offset: the zeroed words say where the runtime's data starts.
        public static string ArrayOffsets()
        {
            var r = new System.Text.StringBuilder();
            { var a = new Pixel[3]; for (int k = 0; k < 3; k++) a[k].Value = 10 * k + 1; Array.Clear(a, 0, 1);
              r.Append("S4:"); for (int k = 0; k < 3; k++) r.Append(a[k].Value + "|"); }
            { var a = new Pair8[3]; for (int k = 0; k < 3; k++) { a[k].A = 10 * k + 1; a[k].B = 10 * k + 2; } Array.Clear(a, 0, 1);
              r.Append(" S8:"); for (int k = 0; k < 3; k++) r.Append(a[k].A + "," + a[k].B + "|"); }
            { var a = new S8L[3]; for (int k = 0; k < 3; k++) a[k].A = ((long)(10 * k + 1) << 32) | (uint)(10 * k + 2); Array.Clear(a, 0, 1);
              r.Append(" S8L:"); for (int k = 0; k < 3; k++) r.Append((a[k].A >> 32) + "," + (a[k].A & 0xFFFFFFFF) + "|"); }
            { var a = new Triple[3]; for (int k = 0; k < 3; k++) { a[k].A = 10 * k + 1; a[k].B = 10 * k + 2; a[k].C = 10 * k + 3; } Array.Clear(a, 0, 1);
              r.Append(" S12:"); for (int k = 0; k < 3; k++) r.Append(a[k].A + "," + a[k].B + "," + a[k].C + "|"); }
            { var a = new S16[3]; for (int k = 0; k < 3; k++) { a[k].A = 10 * k + 1; a[k].B = 10 * k + 2; a[k].C = 10 * k + 3; a[k].D = 10 * k + 4; } Array.Clear(a, 0, 1);
              r.Append(" S16:"); for (int k = 0; k < 3; k++) r.Append(a[k].A + "," + a[k].B + "," + a[k].C + "," + a[k].D + "|"); }
            { var a = new S16D[3]; for (int k = 0; k < 3; k++) { a[k].A = 10 * k + 1; a[k].B = 10 * k + 2; } Array.Clear(a, 0, 1);
              r.Append(" S16D:"); for (int k = 0; k < 3; k++) r.Append(a[k].A + "," + a[k].B + "|"); }
            { var a = new GEnt<int, int>[3]; for (int k = 0; k < 3; k++) { a[k].hash = 10 * k + 1; a[k].next = 10 * k + 2; a[k].key = 10 * k + 3; a[k].value = 10 * k + 4; } Array.Clear(a, 0, 1);
              r.Append(" G16:"); for (int k = 0; k < 3; k++) r.Append(a[k].hash + "," + a[k].next + "," + a[k].key + "," + a[k].value + "|"); }
            { var a = new S20[3]; for (int k = 0; k < 3; k++) { a[k].A = 10 * k + 1; a[k].B = 10 * k + 2; a[k].C = 10 * k + 3; a[k].D = 10 * k + 4; a[k].E = 10 * k + 5; } Array.Clear(a, 0, 1);
              r.Append(" S20:"); for (int k = 0; k < 3; k++) r.Append(a[k].A + "," + a[k].B + "," + a[k].C + "," + a[k].D + "," + a[k].E + "|"); }
            { var a = new S24L[3]; for (int k = 0; k < 3; k++) { a[k].A = 10 * k + 1; a[k].B = 10 * k + 2; a[k].C = 10 * k + 3; } Array.Clear(a, 0, 1);
              r.Append(" S24L:"); for (int k = 0; k < 3; k++) r.Append(a[k].A + "," + a[k].B + "," + a[k].C + "|"); }
            { var a = new S12G<int>[3]; for (int k = 0; k < 3; k++) { a[k].A = 10 * k + 1; a[k].B = 10 * k + 2; a[k].C = 10 * k + 3; } Array.Clear(a, 0, 1);
              r.Append(" G12:"); for (int k = 0; k < 3; k++) r.Append(a[k].A + "," + a[k].B + "," + a[k].C + "|"); }
            { var a = new GEnt<string, int>[3]; for (int k = 0; k < 3; k++) { a[k].hash = 10 * k + 1; a[k].next = 10 * k + 2; a[k].key = "k"; a[k].value = 10 * k + 4; } Array.Clear(a, 0, 1);
              r.Append(" G16R:"); for (int k = 0; k < 3; k++) r.Append(a[k].hash + "," + a[k].next + "," + (a[k].key == null ? 0 : 3) + "," + a[k].value + "|"); }
            { var a = new Named[3]; for (int k = 0; k < 3; k++) { a[k].Name = "n"; a[k].Id = 10 * k + 2; a[k].Tag = "t"; } Array.Clear(a, 0, 1);
              r.Append(" S12R:"); for (int k = 0; k < 3; k++) r.Append((a[k].Name == null ? 0 : 1) + "," + a[k].Id + "," + (a[k].Tag == null ? 0 : 3) + "|"); }
            return r.ToString();
        }

        // Multi-dimensional arrays: constructed through the runtime's helper, read and written through its accessors.
        public static long MultiDim(int n)
        {
            int[,] grid = new int[n, n + 1];
            for (int i = 0; i < n; i++) for (int j = 0; j < n + 1; j++) grid[i, j] = i * 10 + j;
            string[,,] names = new string[2, 2, 2];
            names[1, 0, 1] = "x";
            long sum = 0;
            for (int i = 0; i < grid.GetLength(0); i++) for (int j = 0; j < grid.GetLength(1); j++) sum += grid[i, j];
            return sum + grid.Length * 1000 + names.Length * 100000 + (names[1, 0, 1] == "x" ? 1 : 0) * 1000000 + grid.Rank * 10000000;
        }

        // A struct copied through pointers (cpobj, or ldobj and stobj).
        public static unsafe long StructCopy()
        {
            Triple a = new Triple { A = 1, B = 2, C = 3 };
            Triple b;
            Triple* pa = &a, pb = &b;
            *pb = *pa;
            return b.A + b.B * 10 + b.C * 100;
        }

        public static long EventReflectionGc()
        {
            return CountEvents(typeof(Eventful<int>), 100) + CountEvents(typeof(Eventful<long>), 700) +
                CountEvents(typeof(Eventful<string>), 1300) + CountEvents(typeof(Eventful<byte>), 1900) +
                CountEvents(typeof(Eventful<short>), 2500) + CountEvents(typeof(Eventful<double>), 3100) +
                CountEvents(typeof(Eventful<object>), 3700) + CountEvents(typeof(Eventful<float>), 4300) +
                CountEvents(typeof(Eventful<char>), 4900) + CountEvents(typeof(Eventful<bool>), 5500) +
                CountEvents(typeof(Eventful<uint>), 6100) + CountEvents(typeof(Eventful<ulong>), 6700);
        }

        public static long EventReflection()
        {
            long t = 0;
            for (int round = 0; round < 3; round++)
            {
                Garbage(500);
                System.Reflection.EventInfo[] events = typeof(Eventful).GetEvents();
                t += events.Length;
                System.Reflection.EventInfo e = typeof(Eventful).GetEvent("E7");
                if (e != null && e.Name == "E7") t += 100;
            }
            return t;
        }

        public static long Strings(int n)
        {
            string a = new string('x', n);
            char[] cs = { 'a', 'b', 'c' };
            string b = new string(cs, 1, 2);
            return a.Length * 10 + b.Length + (b == "bc" ? 100 : 0);
        }

        public static long BigStruct(string text)
        {
            Wide w = new Wide();
            w.A = 1; w.B = 2; w.C = 3; w.D = 4; w.E = 5; w.F = 6; w.G = 7; w.H = 8; w.I = 9;
            w.Name = text;
            Wide copy = w;
            Garbage(2000);
            return copy.Sum() + copy.Name.Length * 1000;
        }

        // Floating-point arguments and value types: arguments, results (in r3 or through a buffer), fields, boxing.
        public static double FloatArgs(double a, int b, double c, float d)
        {
            return a * b + c + d;
        }

        public static double CallFloatArgs()
        {
            return FloatArgs(1.5, 3, 2.5, 0.5f);
        }

        public static Vec Scale(Vec v, double k)
        {
            return new Vec(v.X * k, v.Y * k);
        }

        public static double VecMath()
        {
            Vec a = new Vec(1, 2);
            Vec b = new Vec(3, 4);
            Vec c = a.Add(b);
            return c.Length2 + Scale(c, 2).X;
        }

        public static Pixel Brighter(Pixel p)
        {
            Pixel q;
            q.Value = p.Value + 1;
            return q;
        }

        public static Triple Rotate(Triple t)
        {
            return new Triple { A = t.B, B = t.C, C = t.A };
        }

        public static int SmallStructs()
        {
            Pixel p = Brighter(new Pixel { Value = 41 });
            Triple t = Rotate(new Triple { A = 1, B = 2, C = 3 });
            return p.Value * 1000 + t.A * 100 + t.B * 10 + t.C;
        }

        public static double StructFields()
        {
            Body body = new Body();
            body.Position = new Vec(3, 4);
            body.Position.X += 1;
            Vec copy = body.Position;
            copy.Y = 10;
            return body.Position.X * 100 + body.Position.Y + copy.Y;
        }

        public static double PointRect()
        {
            System.Windows.Rect r = new System.Windows.Rect(1, 2, 3, 4);
            System.Windows.Point p = new System.Windows.Point(2, 3);
            return (r.Contains(p) ? 100 : 0) + r.Right + r.Bottom;
        }

        public static double BoxStruct()
        {
            object boxed = new Vec(5, 6);
            return ((Vec)boxed).Y;
        }

        // Interface calls: a method and a property through an interface, using, foreach over an iterator.
        public static int Interfaces()
        {
            ICounter c = new Counter();
            c.Next();
            c.Next();
            return c.Next() + c.Total;
        }

        public static int Using(Counter counter)
        {
            using (counter)
            {
                counter.Next();
            }
            return counter.Disposed * 10 + counter.Total / 100;
        }

        private static IEnumerable<int> Squares(int n)
        {
            for (int i = 0; i < n; i++)
                yield return i * i;
        }

        public static int Foreach(int n)
        {
            int sum = 0;
            foreach (int x in Squares(n))
                sum += x;
            return sum;
        }
    }

    public abstract class Shape
    {
        public abstract int Area();
    }

    public class Square : Shape
    {
        private int side;
        public Square(int side) { this.side = side; }
        public override int Area() { return side * side; }
    }

    public class Rectangle : Shape
    {
        private int width, height;
        public Rectangle(int width, int height) { this.width = width; this.height = height; }
        public override int Area() { return width * height; }
    }

    public class Node
    {
        public int Value;
        public Node Next;
        public Node(int value, Node next) { Value = value; Next = next; }
    }

    public interface ICounter
    {
        int Next();
        int Total { get; }
    }

    public class Counter : ICounter, IDisposable
    {
        private int count;
        public int Disposed;
        public int Next() { return ++count; }
        public int Total { get { return count * 100; } }
        public void Dispose() { Disposed++; }
    }

    public delegate int IntOp(int x);

    public delegate TResult Map<T, TResult>(T value);

    public static class Cache<T>
    {
        public static int Count;
        public static T Last;

        public static T Remember(T value)
        {
            Count++;
            Last = value;
            return Last;
        }
    }

    public class Box<T>
    {
        public T Value;

        public int Tag()
        {
            Cache<T>.Count += 1;
            return Cache<T>.Count;
        }
    }

    public class Adder
    {
        public T Last<T>(T a, T b) { return Tests.Pick(a, b, false); }
        private int n;
        public Adder(int n) { this.n = n; }
        public virtual int Apply(int x) { return x + n; }
        public int Times(int x) { return x * n; }
    }

    public static class Holder
    {
        public static long Big = 1L << 40;
        public static double Ratio = 1.5;
        public static int Initialized;
        public static TimeSpan Elapsed = TimeSpan.FromSeconds(1);
        public static Vec Origin = new Vec(1, 2);
        public static Named Last;

        static Holder()
        {
            Initialized = 100;
        }
    }

    // Calling-convention probes: the JIT compiles XNGenR<n> to return r<n> and XNGenF<n> to return f<n>.
    public struct Pair
    {
        public int A, B;
    }

    // XNGenS<hex> returns the word at the caller's stack pointer + 0x<hex>: where the 9th and 10th arguments land.
    public static class ProbeStack
    {
        public static int XNGenS18(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS1C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS20(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS24(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS28(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS2C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS30(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS34(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS38(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS3C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS40(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS44(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS48(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS4C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS50(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS54(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS58(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS5C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS60(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS64(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS68(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS6C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS70(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS74(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS78(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS7C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS80(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS84(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS88(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS8C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS90(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS94(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS98(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenS9C(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenSA0(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenSA4(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenSA8(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
        public static int XNGenSAC(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) { return 0; }
    }

    public static class ProbeDoubles
    {
        public static long XNGenR3(double d, int i, double e, int j) { return 0; }
        public static long XNGenR4(double d, int i, double e, int j) { return 0; }
        public static long XNGenR5(double d, int i, double e, int j) { return 0; }
        public static long XNGenR6(double d, int i, double e, int j) { return 0; }
        public static long XNGenR7(double d, int i, double e, int j) { return 0; }
        public static double XNGenF1(double d, int i, double e, int j) { return 0; }
        public static double XNGenF2(double d, int i, double e, int j) { return 0; }
        public static double XNGenF3(double d, int i, double e, int j) { return 0; }
    }
    public static class ProbePoint
    {
        public static long XNGenR3(System.Windows.Point p, int m) { return 0; }
        public static long XNGenR4(System.Windows.Point p, int m) { return 0; }
        public static long XNGenR5(System.Windows.Point p, int m) { return 0; }
        public static long XNGenR6(System.Windows.Point p, int m) { return 0; }
        public static long XNGenR7(System.Windows.Point p, int m) { return 0; }
        public static double XNGenF1(System.Windows.Point p, int m) { return 0; }
        public static double XNGenF2(System.Windows.Point p, int m) { return 0; }
        public static double XNGenF3(System.Windows.Point p, int m) { return 0; }
        public static double XNGenF4(System.Windows.Point p, int m) { return 0; }
    }
    public static class ProbePair
    {
        public static long XNGenR3(Pair s, int m) { return 0; }
        public static long XNGenR4(Pair s, int m) { return 0; }
        public static long XNGenR5(Pair s, int m) { return 0; }
        public static long XNGenR6(Pair s, int m) { return 0; }
        public static double XNGenF1(Pair s, int m) { return 0; }
    }
    public static class ProbeRect
    {
        public static long XNGenR3(int a, System.Windows.Rect r, int m) { return 0; }
        public static long XNGenR4(int a, System.Windows.Rect r, int m) { return 0; }
        public static long XNGenR5(int a, System.Windows.Rect r, int m) { return 0; }
        public static long XNGenR6(int a, System.Windows.Rect r, int m) { return 0; }
        public static long XNGenR7(int a, System.Windows.Rect r, int m) { return 0; }
        public static long XNGenR8(int a, System.Windows.Rect r, int m) { return 0; }
        public static double XNGenF1(int a, System.Windows.Rect r, int m) { return 0; }
        public static double XNGenF2(int a, System.Windows.Rect r, int m) { return 0; }
        public static double XNGenF3(int a, System.Windows.Rect r, int m) { return 0; }
        public static double XNGenF4(int a, System.Windows.Rect r, int m) { return 0; }
        public static double XNGenF5(int a, System.Windows.Rect r, int m) { return 0; }
    }
    public static class ProbeReturns
    {
        public static Pair XNGenW(int m) { return new Pair(); }
        public static System.Windows.Point XNGenV(int m) { return new System.Windows.Point(); }
    }

    public class ProbeInstance
    {
        public int Field = 5;
        public System.Windows.Point XNGenI(int m) { return new System.Windows.Point(); }
    }
    public struct Small
    {
        public int V;
    }

    public struct Twelve
    {
        public int A, B, C;
    }

    public static class ProbeSmall
    {
        public static long XNGenR3(Small s, int m) { return 0; }
        public static long XNGenR4(Small s, int m) { return 0; }
        public static Small XNGenW(int m) { return new Small(); }
    }

    public static class ProbeTwelve
    {
        public static long XNGenR3(Twelve t, int m) { return 0; }
        public static long XNGenR4(Twelve t, int m) { return 0; }
        public static long XNGenR5(Twelve t, int m) { return 0; }
    }

    public enum Color { Red, Green, Blue }

    // A value type holding references: its copies in frames must be reported to the GC.
    public struct Named
    {
        public string Name;
        public int Id;
        public object Tag;
        public Named(string name, int id) { Name = name; Id = id; Tag = null; }
        public int Weight { get { return Name.Length * 10 + Id; } }
    }

    // Value types smaller than a word as structs: boxed (char, short), Nullable<bool> (2 bytes), a struct of
    // three shorts (6 bytes) copied through a field and an array.
    public struct Shorts
    {
        public short A, B, C;
    }

    public class ShortsHolder
    {
        public Shorts Value;
        public int Guard = 0x11223344;
    }

    public struct Vec
    {
        public double X, Y;
        public Vec(double x, double y) { X = x; Y = y; }
        public double Length2 { get { return X * X + Y * Y; } }
        public Vec Add(Vec other) { return new Vec(X + other.X, Y + other.Y); }
    }

    public struct Pixel
    {
        public int Value;
    }

    public struct Pair8
    {
        public int A, B;
    }

    public struct Triple
    {
        public int A, B, C;
    }

    public class Body
    {
        public Vec Position;
    }

    // 76 bytes, sequential: the reference is in word 18, past the 16 words a short mask covers.
    public struct Wide
    {
        public long A, B, C, D, E, F, G, H, I;
        public string Name;

        public long Sum() { return A + B + C + D + E + F + G + H + I; }
    }
}
