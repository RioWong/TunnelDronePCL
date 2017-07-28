#include <stitched_cloud.h>

StitchedCloud::StitchedCloud(PointCloudT::Ptr point_cloud)
{
    stitched_cloud = point_cloud;
    removeOutliers(stitched_cloud, 100, 2);
    downSample(stitched_cloud, 100);
    filterRangeZ(stitched_cloud, 0, 10000);
}

void StitchedCloud::addCloud(PointCloudT::Ptr new_cloud, const TransformData& transformation)
{
    // Use private member functions to register a new cloud to the overall stitched cloud
    // Uses given transformation data to improve the speed of registration

    /*          Pre-Processing          */
    pcl::console::print_highlight("Remove outliers\n");
    removeOutliers(new_cloud, 100, 1);
    pcl::console::print_highlight("Downsample (1)\n");
    downSample(new_cloud, 100);
    pcl::console::print_highlight("Transform\n");
    transform(new_cloud, transformation);
    pcl::console::print_highlight("Passthrough filter\n");
    filterRangeZ(new_cloud, 0, 10000);
    /*          Registration            */
    registerWithSAC(new_cloud, 10);
    registerWithICP(new_cloud, 10);
    *stitched_cloud += *new_cloud;
    /*          Post-Processing         */
    pcl::console::print_highlight("Downsample (2)\n");
    downSample(stitched_cloud, 100);
}

void StitchedCloud::registerWithICP(PointCloudT::Ptr cloud, const int iters)
{
    // Transforms 'cloud' such that it more closely aligns with the stitched cloud
    // Accurate results but slow run time
    pcl::console::print_highlight("Iterative closest point\n");
    pcl::IterativeClosestPoint<PointT, PointT> icp;
    icp.setMaximumIterations(iters);
    icp.setInputSource(cloud);
    icp.setInputTarget(stitched_cloud);
    PointCloudT::Ptr registered_cloud (new PointCloudT());
    icp.align(*cloud);
}

void StitchedCloud::registerWithSAC(PointCloudT::Ptr cloud, const int iters)
{
    // Transforms 'cloud' such that it more closely aligns with the stitched cloud
    // Sampled consensus initial alignment
    // Useful for fast but rough registration

    // Estimate normals in the cloud
    pcl::console::print_highlight("Normals\n");
    pcl::PointCloud<pcl::Normal>::Ptr src_normals (new pcl::PointCloud<pcl::Normal> ());
    pcl::PointCloud<pcl::Normal>::Ptr stitched_normals (new pcl::PointCloud<pcl::Normal> ());
    pcl::NormalEstimationOMP<PointT, pcl::Normal> normal_est;
    pcl::search::KdTree<PointT>::Ptr tree (new pcl::search::KdTree<PointT>);
    normal_est.setSearchMethod(tree);;
    normal_est.setKSearch(100);
    normal_est.setInputCloud(cloud);
    normal_est.compute(*src_normals);
    normal_est.setInputCloud(stitched_cloud);
    normal_est.compute(*stitched_normals);

    // Locate features using the normals and clouds
    pcl::console::print_highlight("Fast point feature histogram\n");
    pcl::FPFHEstimationOMP<PointT, pcl::Normal, pcl::FPFHSignature33> fpfh;
    pcl::PointCloud<pcl::FPFHSignature33>::Ptr src_features (new pcl::PointCloud<pcl::FPFHSignature33> ());
    pcl::PointCloud<pcl::FPFHSignature33>::Ptr stitched_features (new pcl::PointCloud<pcl::FPFHSignature33> ());
    fpfh.setSearchMethod(tree);
    fpfh.setKSearch(250);       // must be larger than value for normals
    fpfh.setInputCloud(cloud);
    fpfh.setInputNormals(src_normals);
    fpfh.compute(*src_features);
    fpfh.setInputCloud(stitched_cloud);
    fpfh.setInputNormals(stitched_normals);
    fpfh.compute(*stitched_features);

    // Use the features found to perform the alignment
    pcl::console::print_highlight("Sample consensus initial alignment\n");
    pcl::SampleConsensusInitialAlignment<PointT, PointT, pcl::FPFHSignature33> sac_ia;
    sac_ia.setMaximumIterations(iters);
    // Stitched
    sac_ia.setInputTarget(stitched_cloud);
    sac_ia.setTargetFeatures(stitched_features);
    // Un-registered cloud
    sac_ia.setInputSource(cloud);
    sac_ia.setSourceFeatures(src_features);
    sac_ia.align(*cloud);
}


/*          Helper functions        */
void removeOutliers(PointCloudT::Ptr cloud, const int num_neighbours, const int stddev)
{
    pcl::StatisticalOutlierRemoval<PointT> sor;
    sor.setInputCloud(cloud);
    sor.setMeanK(num_neighbours);        // The number of neighbours to analyse
    sor.setStddevMulThresh(stddev);      // Standard deviation multiplier
    sor.filter(*cloud);
}

void downSample(PointCloudT::Ptr cloud, const int leaf_size)
{
    // Smaller leaf sizes makes the cloud more accurate but also signficantly slower
    pcl::VoxelGrid<PointT> grid;
    grid.setLeafSize(leaf_size, leaf_size, leaf_size);
    grid.setInputCloud(cloud);
    grid.filter(*cloud);
}

void transform(PointCloudT::Ptr cloud, const TransformData& t)
{
    Eigen::Affine3f transform = Eigen::Affine3f::Identity();
    transform.translation() << t.dx, t.dy, t.dz;
    transform.rotate (Eigen::AngleAxisf (-t.rotx, Eigen::Vector3f::UnitX()));
    transform.rotate (Eigen::AngleAxisf (-t.roty, Eigen::Vector3f::UnitY()));
    transform.rotate (Eigen::AngleAxisf (-t.rotz, Eigen::Vector3f::UnitZ()));
    pcl::transformPointCloud(*cloud, *cloud, transform);
}

void filterRangeZ(PointCloudT::Ptr cloud, const double minZ, const double maxZ)
{
    pcl::PassThrough<PointT> pass;
    pass.setInputCloud(cloud);
    pass.setFilterFieldName("z");
    pass.setFilterLimits(minZ, maxZ);
    pass.filter(*cloud);
}