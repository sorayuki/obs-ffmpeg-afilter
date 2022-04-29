using System.IO;
using System.IO.Compression;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Net.Http;

class Program {
    static async Task DownloadFile(string url, string path) {
        Console.WriteLine($"downloading {url} to {path}");
        using (var hc = new HttpClient()) {
            var res = await hc.GetAsync(url);
            if (res.StatusCode == System.Net.HttpStatusCode.OK) {
                using (var f = File.Create(path)) {
                    await res.Content.CopyToAsync(f);
                }
            }
        }
    }
    static async Task Exec(string prog, string arg) {
        Console.WriteLine($"run {prog} {arg}");
        var psi = new ProcessStartInfo();
        psi.FileName = prog;
        psi.Arguments = arg;
        using (var proc = Process.Start(psi)) {
            await proc!.WaitForExitAsync();
        }
    }
    public static async Task Main(string[] args) {
        var obsver = "27.2.4";
        var depver = "2022-03-16";
        await DownloadFile($"https://github.com/obsproject/obs-studio/archive/refs/tags/{obsver}.zip", "obs-src.zip");
        ZipFile.ExtractToDirectory($"obs-src.zip", $"obs-src-tmp");
        Directory.Move(Directory.EnumerateDirectories("obs-src-tmp").ToArray()[0], "obs-src");
        foreach(var arch in new string[] {"x86", "x64"}) {
            await DownloadFile($"https://cdn-fastly.obsproject.com/downloads/OBS-Studio-{obsver}-Full-{arch}.zip", $"obs-bin-{arch}.zip");
            Directory.CreateDirectory($"obs-bin-{arch}");
            ZipFile.ExtractToDirectory($"obs-bin-{arch}.zip", $"obs-bin-{arch}");
            await DownloadFile($"https://github.com/obsproject/obs-deps/releases/download/win-{depver}/windows-deps-{depver}-{arch}.zip", $"obs-dep-{arch}.zip");
            Directory.CreateDirectory($"deps\\{arch}");
            ZipFile.ExtractToDirectory($"obs-dep-{arch}.zip", $"deps\\{arch}");
            await Exec("cmake.exe", $"-G \"Visual Studio 17 2022\" -A {(arch=="x86"?"Win32":"x64")} -B build_{arch} -S . -DCMAKE_INSTALL_PREFIX=dist");
            await Exec("cmake.exe", $"--build build_{arch} --config Release");
            await Exec("cmake.exe", $"--install build_{arch} --config Release");
        }
    }
}