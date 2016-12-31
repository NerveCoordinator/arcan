-- target_flags
-- @short: Set special frameserver processing flags
-- @inargs: id, flag_val, *toggle*
-- @longdescr: Some parts of frameserver management that may be usefull
-- during very specific purposes are disabled by default. This function
-- can be used to manage such flags in order to get access to more
-- information and control.
-- Optional *toggle* argument is by default set to on, to turn off a
-- specific flag, set *toggle* to 0.
-- @note: flag, TARGET_VSTORE_SYNCH makes sure that there is a local
-- copy of the buffer that is also uploaded to the GPU. This saves a
-- readback in cases where direct access to pixel values are needed.
-- @note: flag, TARGET_SYNCHRONOUS blocks CPU<->GPU transfers until
-- the transfer has been marked as completed. This will stall the
-- rendering pipeline and possibly incur a heavy performance impact.
-- @note: flag, TARGET_NOALPHA manually sets the alpha field for new
-- frames to 0xff. A faster way of disabling (that would require less
-- memory operations) alpha blending for untrusted sources would be
-- to associate the respective VIDs with a shader that doesn't use or
-- lookup alpha value.
-- @note: flag: TARGET_AUTOCLOCK is on by default and provides a
-- standardd implementation for some of the autoclock requests that
-- are then filtered and not forwarded to the eventhandler.
-- @note: flag: TARGET_VERBOSE is off by default. Turning it on
-- will emit additional data about delivered or dropped frames.
-- @note: flag: TARGET_NOBUFFERPASS applies to plaforms where accelerated
-- buffer passing is supported. This can be disabled statically/globally
-- through a platform specific environment variable, and dynamically by setting
-- this flag. The default behavior is to allow buffer passing, but if the
-- graphics (agp) layer gets an invalid buffer, this flag is automatically set
-- to true.
-- @note: flag: TARGET_ALLOWCM allows a client to send/receive color
-- lookup tables for one or several displays. You still need to explicitly
-- specify when- and which tables- are to be sent or retrieved via the
-- ref:video_displaygamma call. The event handler will indicate when
-- gamma tables have been updated.
-- @note: flag: TARGET_ALLOWLODEF allows a client to treat its video buffers
-- in half-depth mode. Similar to TARGET_ALLOWHDR, this means that more
-- advanced features than compositing (accessing / manipulating store,
-- direct-feed share and so on) should be avoided.
-- @note: flag: TARGET_ALLOWHDR allows a client to treat its video buffers
-- in f16-mode. This is an advanced graphics format for working with sources
-- that has a dynamic range. Most graphics applications becomes more costly,
-- compositing multiple sources on a fixed-range display.
-- @note: flag: TARGET_ALLOWVECTOR allows a client to treat its video buffers
-- in vector mode. This converts the contents of its buffers to work as
-- textures, and metadata for specifying content in a GPU friendly triangle-
-- soup format. Like with allow-lodef/hdr, this will not work well with
-- feedtargets or directly accessing storage.
-- @group: targetcontrol
-- @cfunction: targetflags
-- @related:
function main()
#ifdef MAIN
	a = launch_avfeed("", "avfeed", function(source, status)
		for k,v in pairs(status) do
			print(k, v);
		end
	end);
	target_flag(a, TARGET_VERBOSE);
	target_flag(a, TARGET_SYNCHRONOUS);
	target_flag(a, TARGET_NOALPHA);
	target_flag(a, TARGET_AUTOCLOCK);
	target_flag(a, TARGET_NOBUFFERPASS);
#endif

#ifdef ERROR
	a = null_surface(64, 64);
	target_flag(a, FLAG_SYNCHRONOUS);
#endif

#ifdef ERROR2
	a = launch_avfeed("", "avfeed", function() end);
	target_flag(a, 10000);
#endif
end
