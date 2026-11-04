namespace NGenProbe
{
    // Getters the native image compiler handles. XNGenMagic has the same IL as Value, but xclrjit compiles a getter
    // with that name to return 0x5EED (24301): seeing that number proves the native image's code ran rather than the
    // IL interpreter.
    public class Probe
    {
        private int value = 7;
        private string text = "seven";

        public int Value { get { return value; } }
        public string Text { get { return text; } }
        public int XNGenMagic { get { return value; } }
    }
}
