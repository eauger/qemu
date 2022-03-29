iommu_ss = ss.source_set()
iommu_ss.add(files(
  'iommufd.c',
))

specific_ss.add_all(when: 'CONFIG_IOMMUFD', if_true: iommu_ss)
