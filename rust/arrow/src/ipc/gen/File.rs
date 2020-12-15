// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#![allow(dead_code)]
#![allow(unused_imports)]

use crate::ipc::gen::Schema::*;
use flatbuffers::EndianScalar;
use std::{cmp::Ordering, mem};
// automatically generated by the FlatBuffers compiler, do not modify

// struct Block, aligned to 8
#[repr(C, align(8))]
#[derive(Clone, Copy, PartialEq)]
pub struct Block {
    offset_: i64,
    metaDataLength_: i32,
    padding0__: u32,
    bodyLength_: i64,
} // pub struct Block
impl std::fmt::Debug for Block {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        f.debug_struct("Block")
            .field("offset", &self.offset())
            .field("metaDataLength", &self.metaDataLength())
            .field("bodyLength", &self.bodyLength())
            .finish()
    }
}

impl flatbuffers::SafeSliceAccess for Block {}
impl<'a> flatbuffers::Follow<'a> for Block {
    type Inner = &'a Block;
    #[inline]
    fn follow(buf: &'a [u8], loc: usize) -> Self::Inner {
        <&'a Block>::follow(buf, loc)
    }
}
impl<'a> flatbuffers::Follow<'a> for &'a Block {
    type Inner = &'a Block;
    #[inline]
    fn follow(buf: &'a [u8], loc: usize) -> Self::Inner {
        flatbuffers::follow_cast_ref::<Block>(buf, loc)
    }
}
impl<'b> flatbuffers::Push for Block {
    type Output = Block;
    #[inline]
    fn push(&self, dst: &mut [u8], _rest: &[u8]) {
        let src = unsafe {
            ::std::slice::from_raw_parts(self as *const Block as *const u8, Self::size())
        };
        dst.copy_from_slice(src);
    }
}
impl<'b> flatbuffers::Push for &'b Block {
    type Output = Block;

    #[inline]
    fn push(&self, dst: &mut [u8], _rest: &[u8]) {
        let src = unsafe {
            ::std::slice::from_raw_parts(*self as *const Block as *const u8, Self::size())
        };
        dst.copy_from_slice(src);
    }
}

impl Block {
    pub fn new(_offset: i64, _metaDataLength: i32, _bodyLength: i64) -> Self {
        Block {
            offset_: _offset.to_little_endian(),
            metaDataLength_: _metaDataLength.to_little_endian(),
            bodyLength_: _bodyLength.to_little_endian(),

            padding0__: 0,
        }
    }
    /// Index to the start of the RecordBlock (note this is past the Message header)
    pub fn offset(&self) -> i64 {
        self.offset_.from_little_endian()
    }
    /// Length of the metadata
    pub fn metaDataLength(&self) -> i32 {
        self.metaDataLength_.from_little_endian()
    }
    /// Length of the data (this is aligned so there can be a gap between this and
    /// the metadata).
    pub fn bodyLength(&self) -> i64 {
        self.bodyLength_.from_little_endian()
    }
}

pub enum FooterOffset {}
#[derive(Copy, Clone, PartialEq)]

/// ----------------------------------------------------------------------
/// Arrow File metadata
///
pub struct Footer<'a> {
    pub _tab: flatbuffers::Table<'a>,
}

impl<'a> flatbuffers::Follow<'a> for Footer<'a> {
    type Inner = Footer<'a>;
    #[inline]
    fn follow(buf: &'a [u8], loc: usize) -> Self::Inner {
        Self {
            _tab: flatbuffers::Table { buf, loc },
        }
    }
}

impl<'a> Footer<'a> {
    #[inline]
    pub fn init_from_table(table: flatbuffers::Table<'a>) -> Self {
        Footer { _tab: table }
    }
    #[allow(unused_mut)]
    pub fn create<'bldr: 'args, 'args: 'mut_bldr, 'mut_bldr>(
        _fbb: &'mut_bldr mut flatbuffers::FlatBufferBuilder<'bldr>,
        args: &'args FooterArgs<'args>,
    ) -> flatbuffers::WIPOffset<Footer<'bldr>> {
        let mut builder = FooterBuilder::new(_fbb);
        if let Some(x) = args.custom_metadata {
            builder.add_custom_metadata(x);
        }
        if let Some(x) = args.recordBatches {
            builder.add_recordBatches(x);
        }
        if let Some(x) = args.dictionaries {
            builder.add_dictionaries(x);
        }
        if let Some(x) = args.schema {
            builder.add_schema(x);
        }
        builder.add_version(args.version);
        builder.finish()
    }

    pub const VT_VERSION: flatbuffers::VOffsetT = 4;
    pub const VT_SCHEMA: flatbuffers::VOffsetT = 6;
    pub const VT_DICTIONARIES: flatbuffers::VOffsetT = 8;
    pub const VT_RECORDBATCHES: flatbuffers::VOffsetT = 10;
    pub const VT_CUSTOM_METADATA: flatbuffers::VOffsetT = 12;

    #[inline]
    pub fn version(&self) -> MetadataVersion {
        self._tab
            .get::<MetadataVersion>(Footer::VT_VERSION, Some(MetadataVersion::V1))
            .unwrap()
    }
    #[inline]
    pub fn schema(&self) -> Option<Schema<'a>> {
        self._tab
            .get::<flatbuffers::ForwardsUOffset<Schema<'a>>>(Footer::VT_SCHEMA, None)
    }
    #[inline]
    pub fn dictionaries(&self) -> Option<&'a [Block]> {
        self._tab
            .get::<flatbuffers::ForwardsUOffset<flatbuffers::Vector<Block>>>(
                Footer::VT_DICTIONARIES,
                None,
            )
            .map(|v| v.safe_slice())
    }
    #[inline]
    pub fn recordBatches(&self) -> Option<&'a [Block]> {
        self._tab
            .get::<flatbuffers::ForwardsUOffset<flatbuffers::Vector<Block>>>(
                Footer::VT_RECORDBATCHES,
                None,
            )
            .map(|v| v.safe_slice())
    }
    /// User-defined metadata
    #[inline]
    pub fn custom_metadata(
        &self,
    ) -> Option<flatbuffers::Vector<'a, flatbuffers::ForwardsUOffset<KeyValue<'a>>>> {
        self._tab.get::<flatbuffers::ForwardsUOffset<
            flatbuffers::Vector<flatbuffers::ForwardsUOffset<KeyValue<'a>>>,
        >>(Footer::VT_CUSTOM_METADATA, None)
    }
}

pub struct FooterArgs<'a> {
    pub version: MetadataVersion,
    pub schema: Option<flatbuffers::WIPOffset<Schema<'a>>>,
    pub dictionaries: Option<flatbuffers::WIPOffset<flatbuffers::Vector<'a, Block>>>,
    pub recordBatches: Option<flatbuffers::WIPOffset<flatbuffers::Vector<'a, Block>>>,
    pub custom_metadata: Option<
        flatbuffers::WIPOffset<
            flatbuffers::Vector<'a, flatbuffers::ForwardsUOffset<KeyValue<'a>>>,
        >,
    >,
}
impl<'a> Default for FooterArgs<'a> {
    #[inline]
    fn default() -> Self {
        FooterArgs {
            version: MetadataVersion::V1,
            schema: None,
            dictionaries: None,
            recordBatches: None,
            custom_metadata: None,
        }
    }
}
pub struct FooterBuilder<'a: 'b, 'b> {
    fbb_: &'b mut flatbuffers::FlatBufferBuilder<'a>,
    start_: flatbuffers::WIPOffset<flatbuffers::TableUnfinishedWIPOffset>,
}
impl<'a: 'b, 'b> FooterBuilder<'a, 'b> {
    #[inline]
    pub fn add_version(&mut self, version: MetadataVersion) {
        self.fbb_.push_slot::<MetadataVersion>(
            Footer::VT_VERSION,
            version,
            MetadataVersion::V1,
        );
    }
    #[inline]
    pub fn add_schema(&mut self, schema: flatbuffers::WIPOffset<Schema<'b>>) {
        self.fbb_
            .push_slot_always::<flatbuffers::WIPOffset<Schema>>(
                Footer::VT_SCHEMA,
                schema,
            );
    }
    #[inline]
    pub fn add_dictionaries(
        &mut self,
        dictionaries: flatbuffers::WIPOffset<flatbuffers::Vector<'b, Block>>,
    ) {
        self.fbb_.push_slot_always::<flatbuffers::WIPOffset<_>>(
            Footer::VT_DICTIONARIES,
            dictionaries,
        );
    }
    #[inline]
    pub fn add_recordBatches(
        &mut self,
        recordBatches: flatbuffers::WIPOffset<flatbuffers::Vector<'b, Block>>,
    ) {
        self.fbb_.push_slot_always::<flatbuffers::WIPOffset<_>>(
            Footer::VT_RECORDBATCHES,
            recordBatches,
        );
    }
    #[inline]
    pub fn add_custom_metadata(
        &mut self,
        custom_metadata: flatbuffers::WIPOffset<
            flatbuffers::Vector<'b, flatbuffers::ForwardsUOffset<KeyValue<'b>>>,
        >,
    ) {
        self.fbb_.push_slot_always::<flatbuffers::WIPOffset<_>>(
            Footer::VT_CUSTOM_METADATA,
            custom_metadata,
        );
    }
    #[inline]
    pub fn new(
        _fbb: &'b mut flatbuffers::FlatBufferBuilder<'a>,
    ) -> FooterBuilder<'a, 'b> {
        let start = _fbb.start_table();
        FooterBuilder {
            fbb_: _fbb,
            start_: start,
        }
    }
    #[inline]
    pub fn finish(self) -> flatbuffers::WIPOffset<Footer<'a>> {
        let o = self.fbb_.end_table(self.start_);
        flatbuffers::WIPOffset::new(o.value())
    }
}

impl std::fmt::Debug for Footer<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let mut ds = f.debug_struct("Footer");
        ds.field("version", &self.version());
        ds.field("schema", &self.schema());
        ds.field("dictionaries", &self.dictionaries());
        ds.field("recordBatches", &self.recordBatches());
        ds.field("custom_metadata", &self.custom_metadata());
        ds.finish()
    }
}
#[inline]
pub fn get_root_as_footer<'a>(buf: &'a [u8]) -> Footer<'a> {
    flatbuffers::get_root::<Footer<'a>>(buf)
}

#[inline]
pub fn get_size_prefixed_root_as_footer<'a>(buf: &'a [u8]) -> Footer<'a> {
    flatbuffers::get_size_prefixed_root::<Footer<'a>>(buf)
}

#[inline]
pub fn finish_footer_buffer<'a, 'b>(
    fbb: &'b mut flatbuffers::FlatBufferBuilder<'a>,
    root: flatbuffers::WIPOffset<Footer<'a>>,
) {
    fbb.finish(root, None);
}

#[inline]
pub fn finish_size_prefixed_footer_buffer<'a, 'b>(
    fbb: &'b mut flatbuffers::FlatBufferBuilder<'a>,
    root: flatbuffers::WIPOffset<Footer<'a>>,
) {
    fbb.finish_size_prefixed(root, None);
}
