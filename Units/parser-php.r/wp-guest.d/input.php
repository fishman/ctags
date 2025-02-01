<?php get_header(); ?>

<div id="top">
 <div class="inner">
  <br clear="all" />
<div id="logo">
<h2><a href="<?php bloginfo('url'); ?>"><?php bloginfo('name'); ?></a></h2>
<p>
  <?php bloginfo('description'); ?>
</p>
</div>
<div id="pages">
<ul class="menubar" id="menubar">
  <li><a href="<?php bloginfo('url'); ?>">Accueil</a></li>
  <?php montheme_list_pages( array( 'depth' => 1 ) ); ?>
</ul>
<!--[if !IE]><-->
<script type="text/javascript">
  initMenu (document.getElementById ('menubar'), 0);
</script>
<!--><![endif]-->
</div>
 </div>
</div>

<div id="body">
  <main id="content">

  <?php if (have_posts()) : while (have_posts()) : the_post(); ?><br /><br />

  <article class="post" id="post-<?php the_ID(); ?>">

   <div class="storycontent">

    <h3><a href="<?php the_permalink() ?>" rel="bookmark"><?php the_title(); ?></a></h3>
   
    <div class="storybody"><br /><?php the_content(__('(more...)')); ?></div>
    <p class="postmetadata"></p>
    
   </div>
     <div class="storybody"><?php comments_template(); // Get wp-comments.php template ?></div>
  </article>


  <?php endwhile; else: ?>
  <p><?php _e('Sorry, no posts matched your criteria.'); ?></p>
  <?php endif; ?>

  <?php posts_nav_link(' &#8212; ', __('&laquo; Previous Page'), __('Next Page &raquo;')); ?>

  </main>
  <aside id="left"><?php get_sidebar(); ?></aside>
</div>

<?php get_footer(); ?>
