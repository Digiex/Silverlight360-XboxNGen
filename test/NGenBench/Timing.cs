using System;
using System.Collections.Generic;
using System.Text;

namespace NGenBench
{
    // The timing loops: realistic work of a few hundred ms each on our images, half of it in the library
    // (StringBuilder, Dictionary, List<T>) and half in the bench's own code (fields, floating point). Each returns
    // a checksum MainPage checks, so a timing never comes from a wrong result. Bench-Timing.ps1 runs them on
    // Microsoft's images with this assembly interpreted, on ours interpreted, and on ours compiled.
    public static class Timing
    {
        // Appends "item<i>," n times, emptying the builder every 4000 characters: the characters appended.
        public static int StringBuild(int n)
        {
            StringBuilder sb = new StringBuilder();
            int total = 0;
            for (int i = 0; i < n; i++)
            {
                sb.Append("item").Append(i).Append(',');
                if (sb.Length > 4000)
                {
                    total += sb.ToString().Length;
                    sb.Length = 0;
                }
            }
            return total + sb.Length;
        }

        // Int keys (every third probe a miss) and string keys.
        public static int DictionaryLookups(int rounds)
        {
            Dictionary<int, int> numbers = new Dictionary<int, int>();
            for (int i = 0; i < 10000; i++)
                numbers[i * 7] = i;
            string[] keys = new string[500];
            Dictionary<string, int> names = new Dictionary<string, int>();
            for (int i = 0; i < keys.Length; i++)
            {
                keys[i] = "key" + i;
                names[keys[i]] = i;
            }
            int sum = 0;
            for (int r = 0; r < rounds; r++)
            {
                for (int k = 0; k < 70000; k += 3)
                {
                    int v;
                    if (numbers.TryGetValue(k, out v))
                        sum += v;
                }
                for (int i = 0; i < keys.Length; i++)
                    sum += names[keys[(i * 31) % keys.Length]];
            }
            return sum;
        }

        // foreach and indexed loops over a List<int>.
        public static int ListIterate(int rounds)
        {
            List<int> list = new List<int>();
            for (int i = 0; i < 100000; i++)
                list.Add(i % 1000);
            int sum = 0;
            for (int r = 0; r < rounds; r++)
            {
                foreach (int x in list)
                    sum += x;
                for (int i = 0; i < list.Count; i++)
                    sum = sum * 31 + list[i];
            }
            return sum;
        }

        public class Particle
        {
            public float X, Y, VX, VY;
            public int Bounces;
        }

        // 1000 particles bouncing in a 100 x 100 box: field loads and stores, the bounces counted.
        public static int FieldObjects(int steps)
        {
            Particle[] particles = new Particle[1000];
            for (int i = 0; i < particles.Length; i++)
                particles[i] = new Particle { X = i % 100, Y = i % 37, VX = i % 7 - 3, VY = i % 5 - 2 };
            for (int s = 0; s < steps; s++)
            {
                for (int i = 0; i < particles.Length; i++)
                {
                    Particle p = particles[i];
                    p.X += p.VX;
                    p.Y += p.VY;
                    if (p.X < 0 || p.X > 100)
                    {
                        p.VX = -p.VX;
                        p.Bounces++;
                    }
                    if (p.Y < 0 || p.Y > 100)
                    {
                        p.VY = -p.VY;
                        p.Bounces++;
                    }
                }
            }
            int bounces = 0;
            for (int i = 0; i < particles.Length; i++)
                bounces += particles[i].Bounces;
            return bounces;
        }

        // A width x height Mandelbrot of up to 200 iterations a point: the iterations taken.
        public static int Mandelbrot(int width, int height)
        {
            int total = 0;
            for (int py = 0; py < height; py++)
            {
                double ci = -1.2 + 2.4 * py / height;
                for (int px = 0; px < width; px++)
                {
                    double cr = -2.0 + 3.0 * px / width;
                    double zr = 0, zi = 0;
                    int n = 0;
                    while (n < 200 && zr * zr + zi * zi <= 4.0)
                    {
                        double t = zr * zr - zi * zi + cr;
                        zi = 2.0 * zr * zi + ci;
                        zr = t;
                        n++;
                    }
                    total += n;
                }
            }
            return total;
        }

        // A quarter of the sum of square roots and sines, as a rounded integer.
        public static int MathCalls(int n)
        {
            double sum = 0;
            for (int i = 1; i <= n; i++)
                sum += Math.Sqrt(i) + Math.Sin(i * 0.001);
            return (int)Math.Round(sum / 4);
        }
    }
}
