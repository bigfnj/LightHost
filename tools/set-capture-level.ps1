# Sets the capture level of one named endpoint, in dB, and reads it back.
#
# dB rather than percentage: the Windows slider percentage is not linear in dB,
# so "about 70%" is a guess while "+21 dB" is a number.
param(
    [string]$Match = 'USB audio CODEC',
    [Parameter(Mandatory = $true)][double]$LevelDb
)

$src = @'
using System;
using System.Runtime.InteropServices;

public static class MicSet
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

    [Guid("5CDF2C82-841E-4546-9722-0CF74078229A"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioEndpointVolume
    {
        int RegisterControlChangeNotify(IntPtr notify);
        int UnregisterControlChangeNotify(IntPtr notify);
        int GetChannelCount(out int count);
        int SetMasterVolumeLevel(float leveldB, IntPtr eventContext);
        int SetMasterVolumeLevelScalar(float level, IntPtr eventContext);
        int GetMasterVolumeLevel(out float leveldB);
        int GetMasterVolumeLevelScalar(out float level);
        int SetChannelVolumeLevel(int channel, float leveldB, IntPtr eventContext);
        int SetChannelVolumeLevelScalar(int channel, float level, IntPtr eventContext);
        int GetChannelVolumeLevel(int channel, out float leveldB);
        int GetChannelVolumeLevelScalar(int channel, out float level);
        int SetMute(bool mute, IntPtr eventContext);
        int GetMute(out bool mute);
        int GetVolumeStepInfo(out int step, out int stepCount);
        int VolumeStepUp(IntPtr eventContext);
        int VolumeStepDown(IntPtr eventContext);
        int QueryHardwareSupport(out int mask);
        int GetVolumeRange(out float minDb, out float maxDb, out float incrementDb);
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

    /// Returns a report line. Only touches endpoints whose name contains `match`.
    public static string Set(string match, double db, bool apply)
    {
        var en = (IMMDeviceEnumerator)(new EnumeratorComObject());
        IMMDeviceCollection col;
        en.EnumAudioEndpoints(1, 1, out col);   // capture, active only

        int count;
        col.GetCount(out count);
        var iid = new Guid("5CDF2C82-841E-4546-9722-0CF74078229A");
        string report = "";

        for (int i = 0; i < count; i++)
        {
            IMMDevice dev;
            col.Item(i, out dev);
            var name = NameOf(dev);
            if (name == null || name.IndexOf(match, StringComparison.OrdinalIgnoreCase) < 0)
                continue;

            object o;
            if (dev.Activate(ref iid, 23, IntPtr.Zero, out o) != 0) continue;
            var vol = (IAudioEndpointVolume)o;

            float before, minDb, maxDb, inc, scalarBefore;
            vol.GetMasterVolumeLevel(out before);
            vol.GetMasterVolumeLevelScalar(out scalarBefore);
            vol.GetVolumeRange(out minDb, out maxDb, out inc);

            var target = (float)Math.Max(minDb, Math.Min(maxDb, db));

            if (apply)
                vol.SetMasterVolumeLevel(target, IntPtr.Zero);

            float after, scalarAfter;
            vol.GetMasterVolumeLevel(out after);
            vol.GetMasterVolumeLevelScalar(out scalarAfter);

            report += string.Format(
                "{0}\n  before: {1,6:F1} dB ({2,5:F1}%)\n  after : {3,6:F1} dB ({4,5:F1}%)\n  range : {5:F1} .. {6:F1} dB\n",
                name, before, scalarBefore * 100.0, after, scalarAfter * 100.0, minDb, maxDb);
        }

        return report.Length == 0 ? "no active capture endpoint matched \"" + match + "\"" : report;
    }
}
'@

Add-Type -TypeDefinition $src -ErrorAction Stop
[MicSet]::Set($Match, $LevelDb, $true)
