PRAGMA application_id=1129337418;
PRAGMA user_version=3;
PRAGMA foreign_keys=ON;
CREATE TABLE project (
 id INTEGER PRIMARY KEY CHECK(id=1), format TEXT NOT NULL CHECK(format='compositor-linux'),
 uuid TEXT NOT NULL UNIQUE CHECK(length(uuid)=36), width INTEGER NOT NULL CHECK(width BETWEEN 1 AND 30000),
 height INTEGER NOT NULL CHECK(height BETWEEN 1 AND 30000), resolution REAL NOT NULL CHECK(resolution>0),
 working_space TEXT NOT NULL CHECK(working_space='linear-srgb-extended-v1'),
 saved_revision INTEGER NOT NULL CHECK(saved_revision>=0), required_features_json TEXT NOT NULL DEFAULT '[]',
 selection_json TEXT NOT NULL DEFAULT 'null' CHECK(length(selection_json)<=512),
 CHECK(width*height<=100000000)
) STRICT;
CREATE TABLE profiles (
 uuid TEXT PRIMARY KEY NOT NULL CHECK(length(uuid)=36), role TEXT NOT NULL CHECK(role IN ('source','display','output')),
 icc BLOB NOT NULL CHECK(length(icc) BETWEEN 1 AND 4194304), checksum BLOB NOT NULL CHECK(length(checksum)=32)
) STRICT;
CREATE TABLE assets (
 uuid TEXT PRIMARY KEY NOT NULL CHECK(length(uuid)=36), origin_x INTEGER NOT NULL, origin_y INTEGER NOT NULL,
 width INTEGER NOT NULL CHECK(width BETWEEN 1 AND 30000), height INTEGER NOT NULL CHECK(height BETWEEN 1 AND 30000),
 format TEXT NOT NULL CHECK(format IN ('rgba16f-le','r16f-le','mask-rgba16f-le')), default_value BLOB NOT NULL,
 revision INTEGER NOT NULL CHECK(revision>=0), source_profile TEXT REFERENCES profiles(uuid),
 CHECK(length(default_value)=CASE WHEN format IN ('rgba16f-le','mask-rgba16f-le') THEN 8 ELSE 2 END), CHECK(width*height<=100000000)
) STRICT;
CREATE TABLE masks (
 uuid TEXT PRIMARY KEY NOT NULL CHECK(length(uuid)=36), asset_uuid TEXT NOT NULL REFERENCES assets(uuid),
 enabled INTEGER NOT NULL CHECK(enabled IN (0,1)), linked INTEGER NOT NULL CHECK(linked IN (0,1)),
 transform BLOB NOT NULL CHECK(length(transform)=72), exterior_coverage REAL NOT NULL DEFAULT 1 CHECK(exterior_coverage BETWEEN 0 AND 1)
) STRICT;
CREATE TABLE layers (
 uuid TEXT PRIMARY KEY NOT NULL CHECK(length(uuid)=36), parent_uuid TEXT REFERENCES layers(uuid) DEFERRABLE INITIALLY DEFERRED,
 sibling_order INTEGER NOT NULL CHECK(sibling_order>=0), type TEXT NOT NULL CHECK(type IN ('raster','paint','shape','adjustment','folder')),
 name TEXT NOT NULL CHECK(length(name)<=4096), visible INTEGER NOT NULL CHECK(visible IN (0,1)),
 opacity REAL NOT NULL CHECK(opacity BETWEEN 0 AND 1), blend_mode TEXT NOT NULL CHECK(blend_mode IN
 ('normal','multiply','screen','overlay','darken','lighten','difference','color-dodge','color-burn','hue','saturation','color','luminosity','linear-burn','linear-dodge','soft-light','hard-light','vivid-light','linear-light','pin-light','hard-mix','exclusion','subtract','divide')),
 transform BLOB NOT NULL CHECK(length(transform)=72), asset_uuid TEXT REFERENCES assets(uuid), mask_uuid TEXT REFERENCES masks(uuid),
 clipping_source_uuid TEXT REFERENCES layers(uuid) DEFERRABLE INITIALLY DEFERRED,
 parameters_json TEXT NOT NULL CHECK(length(parameters_json)<=4194304),
 CHECK(parent_uuid IS NULL OR parent_uuid<>uuid), CHECK(clipping_source_uuid IS NULL OR clipping_source_uuid<>uuid),
 CHECK(type<>'folder' OR (blend_mode='normal' AND asset_uuid IS NULL))
) STRICT;
CREATE UNIQUE INDEX layer_siblings ON layers(ifnull(parent_uuid,''),sibling_order);
CREATE TABLE tiles (
 asset_uuid TEXT NOT NULL REFERENCES assets(uuid), tile_x INTEGER NOT NULL, tile_y INTEGER NOT NULL,
 width INTEGER NOT NULL CHECK(width BETWEEN 1 AND 256), height INTEGER NOT NULL CHECK(height BETWEEN 1 AND 256),
 encoding TEXT NOT NULL CHECK(encoding IN ('constant','raw','zstd')), decoded_size INTEGER NOT NULL CHECK(decoded_size BETWEEN 2 AND 524288),
 checksum BLOB NOT NULL CHECK(length(checksum)=32), payload BLOB NOT NULL CHECK(length(payload)<=decoded_size+65536),
 PRIMARY KEY(asset_uuid,tile_x,tile_y)
) STRICT;
CREATE TABLE preview (
 id INTEGER PRIMARY KEY CHECK(id=1), width INTEGER NOT NULL CHECK(width BETWEEN 1 AND 1024),
 height INTEGER NOT NULL CHECK(height BETWEEN 1 AND 1024), png BLOB NOT NULL CHECK(length(png)<=4194304), CHECK(width*height<=1000000)
) STRICT;
