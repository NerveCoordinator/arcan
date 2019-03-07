-- vr_map_limb
-- @short: Create a binding between a 3d object and a vrbridge limb
-- @inargs: vid:bridge, vid:model, int:limbid
-- @inargs: vid:bridge, vid:model, int:limbid, bool:position, bool:orientation
-- @outargs: nil
-- @longdescr: This function is used to bind the position and orientation of
-- a 3D model to the state of a vrbridge derived limb. This means that whenever
-- a new frame is to be produced, it's properties are sampled from the associated
-- limb directly. This approach is chosen to minimize latency and sampling overhead,
-- as many of the underlying sensor techniques have quite high samplerates and
-- not suitable for processing in a scripting VM/ garbage collected context.
-- If position and orientation argument form is provided (default: true on both),
-- only the fields set to true will actually be applied from the limb, the other
-- attributes will be derived/inherited as normal.
-- @note: The targeted model will not behave as a normal object while mapped in
-- regards to transformation chains and other positional/rotational properties,
-- meaning that operations e.g. forward3d_model will produce undefined results.
-- @group: iodev
-- @cfunction: vr_maplimb
-- @related:
-- @example: vrtest
