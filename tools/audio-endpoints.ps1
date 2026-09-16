# Lists every active audio endpoint and which default roles it holds.
#
# Why roles matter: Windows keeps THREE separate defaults per direction --
# Console, Multimedia and Communications. Teams and most softphones follow the
# Communications role, so a chain can be wired perfectly and still be bypassed
# because the calling app is reading a different device than the one you set.
#
# And the reverse hazard: if the default RENDER device is the virtual cable that
# the host writes into, then all system audio -- including the far end of a call
# -- lands in the cable and is sent back as your microphone. That is heard by
# everyone else as feedback.
$src = @'
using System;
using System.Runtime.InteropServices;

public static class Endpoints
{
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    internal class EnumeratorComObject { }

    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceEnumerator
    {
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
        int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
    }

    [Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceCollection
    {
        int GetCount(out int count);
        int Item(int index, out IMMDevice device);
    }

    [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDevice
    {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams,
                     [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        int OpenPropertyStore(int stgmAccess, out IPropertyStore properties);
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        int GetState(out int state);
    }

    [Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IPropertyStore
    {
        int GetCount(out int count);
        int GetAt(int index, out PropertyKey key);
        int GetValue(ref PropertyKey key, out PropVariant value);
        int SetValue(ref PropertyKey key, ref PropVariant value);
        int Commit();
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey { public Guid fmtid; public int pid; }

    [StructLayout(LayoutKind.Explicit)]
    internal struct PropVariant
    {
        [FieldOffset(0)] public short vt;
        [FieldOffset(8)] public IntPtr pointerValue;
    }

    private static string NameOf(IMMDevice dev)
    {
        IPropertyStore store;
        dev.OpenPropertyStore(0, out store);
        var key = new PropertyKey { fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), pid = 14 };
        PropVariant pv;
        store.GetValue(ref key, out pv);
        return Marshal.PtrToStringUni(pv.pointerValue);
    }

    private static string IdOf(IMMDevice dev)
    {
        string id;
        dev.GetId(out id);
        return id;
    }

    public static string Report()
    {
        var en = (IMMDeviceEnumerator)(new EnumeratorComObject());
        var sb = new System.Text.StringBuilder();
        var roles = new string[] { "Console", "Multimedia", "Communications" };

        for (int flow = 0; flow < 2; flow++)
        {
            sb.AppendLine(flow == 0 ? "=== PLAYBACK (render) ===" : "=== RECORDING (capture) ===");

            // Which device holds each default role.
            var defaults = new string[3];
            for (int r = 0; r < 3; r++)
            {
                IMMDevice d;
                if (en.GetDefaultAudioEndpoint(flow, r, out d) == 0 && d != null)
                    defaults[r] = IdOf(d);
            }

            IMMDeviceCollection col;
            en.EnumAudioEndpoints(flow, 1, out col);   // active only
            int count;
            col.GetCount(out count);

            for (int i = 0; i < count; i++)
            {
                IMMDevice dev;
                col.Item(i, out dev);
                var id = IdOf(dev);
                var tags = "";
                for (int r = 0; r < 3; r++)
                    if (defaults[r] == id) tags += " [" + roles[r] + "]";

                sb.AppendLine(string.Format("  {0,-52}{1}", NameOf(dev), tags));
            }
            sb.AppendLine();
        }
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $src -ErrorAction Stop
[Endpoints]::Report()
